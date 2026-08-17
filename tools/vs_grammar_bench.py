#!/usr/bin/env python3
# Copyright (c) 2026, VectorCamp PC
# SPDX-License-Identifier: BSD-3-Clause
"""
vs_grammar_bench.py — benchmark a Vectorscan grammar (vs an optional Eduction baseline) and surface
Hyperscan-performance-guide static findings; optionally apply conservative optimizations.

Pipeline: validate -> detect format -> build vs_grammar_bench if missing -> normalize input ->
run warmup+repeat via the C++ exec -> aggregate -> (optional) Eduction parity -> static analysis ->
JSON (+ optional Markdown). --apply-safe/--apply-risky drive the change loop (Phase 7).

Formats: .hsg and .spec go through the public HS_FLAG_GRAMMAR_REF path (the exec handles both).
XML is best-effort: this fork has no general XML->HSG converter, so .xml reports a structured
`converter_missing` finding rather than faking conversion.

Perf guidance: https://intel.github.io/hyperscan/dev-reference/performance.html
"""
import argparse, csv, json, os, re, shutil, statistics, subprocess, sys, time
from pathlib import Path

PERF_GUIDE = "https://intel.github.io/hyperscan/dev-reference/performance.html"


def warn(msg): print(f"[warn] {msg}", file=sys.stderr)
def info(msg): print(f"[info] {msg}", file=sys.stderr)
def die(msg, code=2):
    print(f"[error] {msg}", file=sys.stderr); sys.exit(code)


# ---------------------------------------------------------------- format + build
def detect_format(path, fmt):
    if fmt != "auto":
        return fmt
    return {".hsg": "hsg", ".spec": "spec", ".xml": "xml"}.get(Path(path).suffix.lower(), "hsg")


def ensure_bench_exe(args):
    exe = Path(args.bench_exe)
    if exe.exists():
        return str(exe)
    build = Path(args.build_dir)
    if not build.exists():
        info(f"configuring build dir {build}")
        subprocess.run(["cmake", "-S", ".", "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"], check=True)
    info(f"building target {args.target}")
    subprocess.run(["cmake", "--build", str(build), "--target", args.target, "-j"], check=True)
    if exe.exists():
        return str(exe)
    alt = build / "bin" / args.target
    if alt.exists():
        return str(alt)
    die(f"benchmark executable not found after build: {exe} (also tried {alt})")


def normalize_grammar(path, fmt):
    """Return (usable_path or None, finding or None). Never fakes support."""
    if fmt == "hsg":
        return path, None
    if fmt == "spec":
        return path, None  # .spec is accepted by the exec via HS_FLAG_GRAMMAR_REF on this branch
    if fmt == "xml":
        # Auto-convert Eduction grammar XML -> .hsg via tools/xml_to_hsg.py (same dir). Faithful for
        # literal dicts + regex + inlined (?A:name) composition; empty/oversized entities are reported,
        # not faked.
        try:
            import tempfile
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import xml_to_hsg
            outdir = tempfile.mkdtemp(prefix="xml2hsg_")
            hsg, dicts, report = xml_to_hsg.convert(path, outdir, som=True, lit_threshold=1000)
        except xml_to_hsg.ConvertError as e:
            return None, {"type": "unsupported", "format": "xml", "reason": "xml_not_convertible",
                          "detail": str(e)}
        except Exception as e:  # ET.ParseError etc.
            return None, {"type": "unsupported", "format": "xml", "reason": "xml_conversion_failed",
                          "detail": str(e)}
        outp = str(Path(outdir) / (Path(path).stem + ".hsg"))
        Path(outp).write_text(hsg)
        emitted = report["emitted"]
        if not [e for e in emitted if not e.get("skipped")]:
            return None, {"type": "unsupported", "format": "xml", "reason": "xml_no_convertible_entities",
                          "detail": "all public entities were empty/unconvertible (decompiled XML carried "
                                    "no data to convert)", "emitted": emitted}
        return outp, {"type": "converted", "format": "xml", "converted_from": "xml",
                      "converted_hsg": outp, "emitted": emitted,
                      "note": "XML auto-converted to .hsg by tools/xml_to_hsg.py (SOM on). "
                              "Oversized inlined compositions may still hit Hyperscan's pattern-length limit."}
    return None, {"type": "unsupported", "format": fmt, "reason": "unknown_format",
                  "detail": f"unrecognized grammar format '{fmt}'"}


# ---------------------------------------------------------------- run + aggregate
def run_bench_once(exe, grammar, corpus):
    p = subprocess.run([exe, "--grammar", grammar, "--corpus", corpus], capture_output=True, text=True)
    try:
        obj = json.loads(p.stdout) if p.stdout.strip() else {}
    except json.JSONDecodeError:
        obj = {}
    if "vectorscan" not in obj and "error" not in obj:
        obj = {"error": "bench_exec_failed", "rc": p.returncode, "stderr": p.stderr.strip()[:500]}
    return obj


def aggregate(runs):
    vs = [r["vectorscan"] for r in runs if "vectorscan" in r]
    if not vs:
        return None
    def stat(k):
        xs = [v[k] for v in vs]
        return {"median": round(statistics.median(xs), 4), "min": round(min(xs), 4), "max": round(max(xs), 4)}
    first = vs[0]
    out = {
        "runs": len(vs),
        "compile_ms": stat("compile_ms"),
        "scan_ms": stat("scan_ms"),
        "throughput_mb_s": stat("throughput_mb_s"),
        "matches": first["matches"],
        "corpus_bytes": first["corpus_bytes"],
        "detections": first.get("detections", []),
        "detections_truncated": first.get("detections_truncated", False),
    }
    if "rss_mb" in first:
        out["rss_mb"] = stat("rss_mb")
    return out


# ---------------------------------------------------------------- Eduction parity
def _norm_ed_rows(rows, corpus_name=None):
    """Drop pure-space tokens and strip one trailing space (same normalization as prior deliverables).
    If corpus_name is given, keep ONLY rows whose `file` column matches it — an Eduction CSV often
    holds detections for several corpus files concatenated (e.g. a3__dense + a3__sparse), and comparing
    those against a single scanned corpus would count the other file's rows as spurious FNs."""
    out = []
    for row in rows:
        fn, ent, s, e, text = row if len(row) == 5 else ("",) + tuple(row)
        if corpus_name and fn and Path(fn).name != corpus_name:
            continue  # this row belongs to a different corpus file
        if text.strip() == "" and text != "":
            continue  # pure-space artifact token
        if text.endswith(" ") and e > s:
            e -= 1
        out.append((s, e))
    return set(out)


def leftmost_longest(pairs):
    """Reduce a set of (start,end) spans to non-overlapping leftmost-longest — Eduction's reporting
    model. Hyperscan reports EVERY end position for a match (e.g. 1173->1203, 1173->1206, 1173->1207);
    this keeps the longest span at each start and drops spans that overlap an already-kept one.
    A principled semantic alignment (documented), NOT a fudge to inflate agreement."""
    spans = sorted(set(pairs), key=lambda p: (p[0], -p[1]))
    kept, last_end = [], -1
    for s, e in spans:
        if s >= last_end:          # non-overlapping: first span at this start is the longest
            kept.append((s, e))
            last_end = e
    return set(kept)


def byte_to_char(pairs, corpus_bytes):
    """Map byte offsets -> character offsets. Eduction reports CHARACTER offsets; Hyperscan reports
    BYTE offsets. On multibyte (UTF-8) corpora these differ (e.g. 'Ö' is 1 char / 2 bytes), so a
    byte-space span never equals the char-space Eduction span. Returns char-space (start,end) spans."""
    # prefix[i] = number of characters in corpus_bytes[:i]. Built once, indexed per offset.
    prefix = [0] * (len(corpus_bytes) + 1)
    chars = 0
    for i, b in enumerate(corpus_bytes):
        # count a character at each byte that is NOT a UTF-8 continuation byte (0b10xxxxxx)
        if (b & 0xC0) != 0x80:
            chars += 1
        prefix[i + 1] = chars
    def c(o):
        o = max(0, min(o, len(corpus_bytes)))
        return prefix[o]
    return set((c(s), c(e)) for (s, e) in pairs)


def _has_multibyte(corpus_bytes):
    return any(b & 0x80 for b in corpus_bytes)


class EductionCsvSchemaError(Exception):
    """Raised when an Eduction CSV has rows but no recognizable start/end columns — so we never
    silently return 0 detections (which would produce a spurious parity=True)."""


def _norm_col(c):
    return c.strip().lower().replace(" ", "_")


def load_eduction_csv(path):
    """Parse an Eduction detections CSV. Column names are normalized (lowercase, trimmed, spaces->
    underscores) so both 'start_offset'/'end_offset' and 'start offset'/'end offset' work.
    Raises EductionCsvSchemaError if data is present but start/end columns are unrecognizable."""
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows:
        return []  # genuinely empty file -> 0 rows (honest)
    header = [_norm_col(c) for c in rows[0]]

    def find(cands):
        for i, h in enumerate(header):
            if h in cands:
                return i
        return -1

    si = find({"start_offset", "start_off", "start", "from"})
    ei = find({"end_offset", "end_off", "end", "to"})
    ent_i = find({"entity", "type", "name"})
    txt_i = find({"text", "match", "matched_text"})
    file_i = find({"file", "filename", "path", "doc", "document"})
    if si < 0 or ei < 0:
        raise EductionCsvSchemaError(
            f"no recognizable start/end columns in header {rows[0]} "
            f"(normalized {header}); expected start_offset/end_offset or 'start offset'/'end offset'")

    out = []
    for r in rows[1:]:
        if len(r) <= max(si, ei):
            continue
        try:
            s, e = int(r[si]), int(r[ei])
        except ValueError:
            continue  # non-numeric row (e.g. stray blank) — skip, schema is still valid
        ent = r[ent_i].split("/")[-1] if 0 <= ent_i < len(r) else ""
        txt = r[txt_i] if 0 <= txt_i < len(r) else ""
        fn = r[file_i] if 0 <= file_i < len(r) else ""
        out.append((fn, ent, s, e, txt))
    return out


def parity_from_csv(vs_dets, ed_rows, corpus_bytes=None, leftmost=True, char_offsets="auto",
                    corpus_name=None):
    """Compare VS spans to Eduction spans. Two documented, principled normalizations align the two
    engines' reporting models before comparing (each recorded in the returned `normalization`):
      - leftmost_longest: collapse Hyperscan's all-end-positions to Eduction's non-overlapping model.
      - char_offsets: map VS byte offsets to character offsets on multibyte corpora (Eduction is char).
    Both are OFF-switchable; neither invents matches."""
    vs = set((d["from"], d["to"]) for d in vs_dets)
    ed = _norm_ed_rows(ed_rows, corpus_name=corpus_name)
    norm = []
    use_char = (char_offsets is True) or (char_offsets == "auto" and corpus_bytes is not None
                                          and _has_multibyte(corpus_bytes))
    if use_char and corpus_bytes is not None:
        vs = byte_to_char(vs, corpus_bytes)
        norm.append("char_offsets")
    if leftmost:
        vs = leftmost_longest(vs)
        ed = leftmost_longest(ed)   # idempotent for Eduction (already non-overlapping); keeps it fair
        norm.append("leftmost_longest")
    fp = sorted(vs - ed)   # in VS, not Eduction
    fn = sorted(ed - vs)   # in Eduction, not VS
    return {
        "available": True, "vs_spans": len(vs), "ed_spans": len(ed),
        "false_positives": len(fp), "false_negatives": len(fn),
        "span_matched": len(vs & ed),
        "parity": (len(fp) == 0 and len(fn) == 0),
        "normalization": norm or ["raw"],
        "fp_examples": fp[:10], "fn_examples": fn[:10],
    }


def run_eduction_bin(edbin, corpus):
    warn(f"--eduction-bin given ({edbin}) but live Eduction execution is environment-specific "
         "(jar + quarantine setup); not run here. Provide --eduction-output CSV for parity.")
    return {"available": False, "reason": "eduction_bin_not_run"}


def load_perf_csv(path):
    """Read a per-engine performance CSV (run_number,wall_ms,cpu_ms,rss_mb,detection). Excludes the
    warmup run 0 and any non-numeric 'avg' summary row; returns median wall_ms / cpu_ms / rss_mb."""
    walls, cpus, rss = [], [], []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            try:
                if int(r["run_number"]) <= 0:       # skip warmup run 0 (and reject 'avg' via ValueError)
                    continue
            except (ValueError, TypeError, KeyError):
                continue
            def num(k, acc):
                try: acc.append(float(r[k]))
                except (ValueError, TypeError, KeyError): pass
            num("wall_ms", walls); num("cpu_ms", cpus); num("rss_mb", rss)
    if not walls:
        return None
    return {"wall_ms": statistics.median(walls),
            "cpu_ms": statistics.median(cpus) if cpus else None,
            "rss_mb": statistics.median(rss) if rss else None,
            "runs": len(walls)}


def head_to_head(vs_bench, ed_perf):
    """VS-vs-Eduction time+memory comparison — the 'optimize' goal: is VS faster and lighter than
    Eduction? VS time = compile_ms + scan_ms (median, this tool's live run); VS memory = peak RSS.
    Eduction time/memory come from its performance CSV. Honest: reports the numbers and the verdict,
    including when VS does NOT win (never asserts a win it didn't measure)."""
    if ed_perf is None:
        return {"available": False, "reason": "eduction_perf_unreadable"}
    vs_wall = vs_bench["compile_ms"]["median"] + vs_bench["scan_ms"]["median"]
    vs_rss = vs_bench.get("rss_mb", {}).get("median")
    ed_wall, ed_rss = ed_perf["wall_ms"], ed_perf.get("rss_mb")
    out = {
        "available": True,
        "vs_wall_ms": round(vs_wall, 4), "eduction_wall_ms": round(ed_wall, 4),
        "time_speedup_x": round(ed_wall / vs_wall, 2) if vs_wall > 0 else None,
        "vs_faster": vs_wall < ed_wall,
        "note": "VS wall = compile_ms + scan_ms (in-process); Eduction wall = wall_ms from its perf CSV "
                "(both are grammar-load/compile + scan of the same corpus).",
    }
    if vs_rss is not None and ed_rss is not None:
        out.update(vs_rss_mb=round(vs_rss, 4), eduction_rss_mb=round(ed_rss, 4),
                   memory_ratio_vs_over_ed=round(vs_rss / ed_rss, 3) if ed_rss > 0 else None,
                   vs_less_memory=vs_rss < ed_rss)
    return out


def compute_parity(args, dets):
    """Single source of truth for parity (used by main + the apply loop). A CSV schema error is
    surfaced as parity 'unavailable' with reason — never a silent 0-row parity=True."""
    if args.eduction_output:
        if not Path(args.eduction_output).exists():
            return {"available": False, "reason": "eduction_output_not_found"}
        try:
            ed = load_eduction_csv(args.eduction_output)
        except EductionCsvSchemaError as e:
            return {"available": False, "reason": "eduction_csv_schema_error", "detail": str(e)}
        corpus_bytes = None
        try:
            corpus_bytes = Path(args.corpus).read_bytes()
        except OSError:
            pass
        return parity_from_csv(dets, ed, corpus_bytes=corpus_bytes,
                               leftmost=not getattr(args, "no_leftmost_longest", False),
                               char_offsets=(False if getattr(args, "no_char_offsets", False) else "auto"),
                               corpus_name=Path(args.corpus).name)
    if args.eduction_bin:
        return run_eduction_bin(args.eduction_bin, args.corpus)
    return {"available": False, "reason": "no_eduction_data"}


def spec_regex_component_finding(path):
    """The exec compiles .spec P-lines as LITERALS. If a .spec's P-lines look like regexes (e.g. the
    addr benchmark specs 'P<TAB>street<TAB>(?i:...)'), warn: they will match literally, not as regexes."""
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError:
        return None
    meta = re.compile(r'\(\?|\\[dwsDWSbB]|\[[^\]]+\]|\{\d|\)[?*+]|[^\\]\|')
    n_p = n_rx = 0
    for ln in lines:
        if ln.startswith("P\t"):
            n_p += 1
            parts = ln.split("\t")
            if len(parts) >= 3 and meta.search(parts[2]):
                n_rx += 1
    if n_p and n_rx >= max(1, n_p // 10):
        return {"severity": "limitation", "kind": "regex_component_spec_unsupported",
                "where": "spec P-lines",
                "detail": (f"{n_rx}/{n_p} P-lines look like REGEX components. The tool compiles .spec P-lines "
                           "as LITERALS (hs_compile_lit_multi), so regex-component specs (e.g. addr benchmark "
                           "specs) are matched literally and are NOT semantically equivalent. Only "
                           "literal-component .spec (broad_list/name style) is supported. Do not claim parity "
                           "for regex-component specs without engine support."),
                "note": "known limitation, not an engine bug"}
    return None


# ---------------------------------------------------------------- static analysis (perf guide)
RE_ID_REGEX = re.compile(r'^\s*(\d+)\s*:\s*/(.*)/([A-Za-z0-9]*)\s*$')  # import-style .hsg: id:/regex/flags


def extract_patterns(path, fmt):
    """Best-effort. Returns (patterns, partial_flag, note). Compose/.spec grammars are literal-based,
    so they yield few/no classic-regex findings — reported honestly, not crashed."""
    try:
        lines = Path(path).read_text(errors="replace").splitlines()
    except OSError as e:
        return [], True, f"could not read grammar: {e}"
    pats = []
    if fmt == "hsg":
        for ln, line in enumerate(lines, 1):
            m = RE_ID_REGEX.match(line)
            if m:
                pats.append({"line": ln, "id": int(m.group(1)), "regex": m.group(2), "flags": m.group(3)})
        note = ("no id:/regex/ patterns found — this looks like a compose (.hsg) grammar of literal "
                "dicts + templates; regex-level static checks mostly N/A" if not pats else "")
        return pats, False, note
    if fmt == "spec":
        return [], False, ".spec is literal components + templates (no classic regexes); regex checks N/A"
    return [], True, f"static extraction not implemented for format '{fmt}'"


def static_findings(pats, note):
    F = []
    def add(sev, kind, detail, where):
        F.append({"severity": sev, "kind": kind, "detail": detail, "where": where,
                  "note": "potential optimization opportunity, not a proven bug"})
    for p in pats:
        rx, where = p["regex"], f"line {p['line']} id {p['id']}"
        if rx.startswith(".*"): add("info", "leading_dotstar", "leading '.*' — prefer an anchored/required literal (perf guide)", where)
        if rx.startswith(".+"): add("info", "leading_dotplus", "leading '.+'", where)
        if not re.search(r'[A-Za-z0-9]{3,}', rx): add("info", "no_required_literal", "no obvious required literal (>=3 chars)", where)
        m = re.search(r'\{\s*(\d+)\s*,\s*(\d+)\s*\}', rx)
        if m and int(m.group(2)) >= 100: add("info", "large_bounded_repeat", f"large bounded repeat {{{m.group(1)},{m.group(2)}}}", where)
        if re.search(r'[+*]\)*[+*]', rx): add("info", "nested_quantifier", "possible nested quantifier", where)
        if 'i' in p["flags"]: add("info", "case_insensitive", "case-insensitive flag on a broad token can enlarge the database", where)
        if 'L' in p["flags"]: add("review_required", "som_leftmost", "SOM_LEFTMOST ('L'): removing it changes start offsets/spans — review_required, never auto", where)
    # alternation: measurement concern only, never "rewrite this"
    big_alts = [p for p in pats if p["regex"].count('|') >= 20]
    for p in big_alts:
        add("info", "large_alternation",
            f"large alternation ({p['regex'].count('|')+1} branches) — investigate compile time / DB size / "
            "memory / state space; do NOT hand-rewrite without benchmark evidence",
            f"line {p['line']} id {p['id']}")
    return F, note


# ---------------------------------------------------------------- apply framework (Phase 7)
def normalize_whitespace(text):
    """Conservative, output-preserving cleanup: strip trailing spaces per line + collapse >2 blank lines.
    Does NOT touch any pattern/flag/literal, so it cannot change compile/scan/detection results."""
    lines = [ln.rstrip() for ln in text.split("\n")]
    out, blanks = [], 0
    for ln in lines:
        if ln == "":
            blanks += 1
            if blanks <= 1:
                out.append(ln)
        else:
            blanks = 0
            out.append(ln)
    return "\n".join(out)


def apply_loop(grammar, fmt, exe, corpus, args, baseline_bench, baseline_parity):
    """v1: implement the safe loop with ONE conservative output-preserving transform (whitespace).
    Risky changes are classified suggest-only and NOT applied (documented, not faked)."""
    report = {"mode": ("risky" if args.apply_risky else "safe"), "applied": [], "suggested_only": []}
    if fmt != "hsg":
        report["note"] = f"auto-apply only operates on .hsg source in v1; format '{fmt}' left unchanged"
        return report

    # --- one SAFE change: whitespace normalization, verified no-op via re-benchmark + parity ---
    src = Path(grammar).read_text()
    new = normalize_whitespace(src)
    if new != src:
        backup = grammar + ".bak"
        shutil.copyfile(grammar, backup)
        Path(grammar).write_text(new)
        after = aggregate([run_bench_once(exe, grammar, corpus) for _ in range(max(1, args.repeat))])
        after_par = None
        if baseline_parity and baseline_parity.get("available"):
            after_par = compute_parity(args, after["detections"])
        parity_ok = (after_par is None) or (not _parity_regressed(baseline_parity, after_par))
        matches_ok = after and after["matches"] == baseline_bench["matches"]
        keep = matches_ok and parity_ok
        change = {
            "file": grammar, "change": "whitespace/comment normalization (output-preserving)",
            "before_matches": baseline_bench["matches"], "after_matches": after["matches"] if after else None,
            "parity_before": _parity_brief(baseline_parity), "parity_after": _parity_brief(after_par),
            "benchmark_before": baseline_bench["scan_ms"], "benchmark_after": after["scan_ms"] if after else None,
            "reason": "safe: does not alter any pattern/flag/literal",
            "kept": keep,
        }
        if keep:
            os.remove(backup)
        else:  # should not happen for a no-op, but honor the rollback contract
            shutil.copyfile(backup, grammar); os.remove(backup)
            change["rolled_back_reason"] = "matches/parity changed unexpectedly"
        report["applied"].append(change)
    else:
        report["note_ws"] = "no whitespace changes needed"

    # --- risky changes: SUGGEST ONLY in v1 (honest; not faked) ---
    report["suggested_only"].append({
        "note": ("Risky regex/flag optimizations (SOM removal, anchoring, DOTALL, SINGLEMATCH, bounded-repeat "
                 "or alternation rewrites, etc.) are NOT auto-applied in this first iteration. They are reported "
                 "as static findings + classified by classify.py as review_required. Full auto-rewriting with "
                 "per-change benchmark+parity gating is a documented follow-up. --apply-risky currently applies "
                 "no risky edits (framework present, conservative behavior).")
    })
    return report


def _parity_brief(par):
    if not par or not par.get("available"):
        return "unavailable"
    return {"parity": par["parity"], "fp": par["false_positives"], "fn": par["false_negatives"]}


def _parity_regressed(base, new):
    if not base or not new or not base.get("available") or not new.get("available"):
        return False
    return (new["false_positives"] > base["false_positives"] or
            new["false_negatives"] > base["false_negatives"] or
            (base["parity"] and not new["parity"]))


# ---------------------------------------------------------------- markdown
def to_markdown(rep):
    L = ["# Grammar benchmark report", ""]
    L.append(f"- grammar: `{rep['grammar']}`  (format: {rep['format']})")
    L.append(f"- corpus: `{rep['corpus']}`")
    b = rep.get("benchmark")
    if b:
        L += ["", "## Vectorscan", "",
              f"- compile_ms (median/min/max): {b['compile_ms']['median']} / {b['compile_ms']['min']} / {b['compile_ms']['max']}",
              f"- scan_ms (median/min/max): {b['scan_ms']['median']} / {b['scan_ms']['min']} / {b['scan_ms']['max']}",
              f"- throughput_mb_s (median): {b['throughput_mb_s']['median']}",
              f"- matches: {b['matches']}  (corpus {b['corpus_bytes']} bytes; runs={b['runs']})"]
        if b.get("rss_mb"):
            L.append(f"- peak RSS MB (median/min/max): {b['rss_mb']['median']} / {b['rss_mb']['min']} / {b['rss_mb']['max']}")
    h = rep.get("efficiency")
    if h and h.get("available"):
        L += ["", "## VS vs Eduction (time + memory)", "",
              f"- **time:** VS {'faster' if h['vs_faster'] else 'SLOWER'} — {h.get('time_speedup_x')}x "
              f"(VS {h['vs_wall_ms']} ms vs Eduction {h['eduction_wall_ms']} ms)"]
        if "vs_rss_mb" in h:
            L.append(f"- **memory:** VS {'lighter' if h['vs_less_memory'] else 'HEAVIER'} — "
                     f"VS {h['vs_rss_mb']} MB vs Eduction {h['eduction_rss_mb']} MB "
                     f"(ratio {h['memory_ratio_vs_over_ed']}x)")
        L.append(f"- _{h['note']}_")
    p = rep.get("parity")
    L += ["", "## Eduction parity", ""]
    if p and p.get("available"):
        L.append(f"- parity: **{p['parity']}**  (matched {p['span_matched']}, FP {p['false_positives']}, FN {p['false_negatives']})")
    else:
        L.append(f"- parity: **unavailable** ({(p or {}).get('reason','no eduction data provided')})")
    L += ["", "## Static findings (perf guide)", ""]
    if rep.get("static_note"):
        L.append(f"- note: {rep['static_note']}")
    for f in rep.get("static_findings", []):
        L.append(f"- [{f['severity']}] {f['kind']} @ {f['where']} — {f['detail']}")
    if not rep.get("static_findings"):
        L.append("- (none)")
    L += ["", f"Perf guide: {PERF_GUIDE}", ""]
    return "\n".join(L)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Benchmark a Vectorscan grammar (optionally vs Eduction) "
                                             "and surface Hyperscan performance-guide findings.")
    ap.add_argument("--grammar", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True, help="JSON report output path")
    ap.add_argument("--format", default="auto", choices=["auto", "hsg", "spec", "xml"])
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--bench-exe", default="build/bin/vs_grammar_bench")
    ap.add_argument("--target", default="vs_grammar_bench")
    ap.add_argument("--eduction-output", help="existing Eduction detections CSV (correctness parity)")
    ap.add_argument("--eduction-perf", help="Eduction performance CSV (run_number,wall_ms,cpu_ms,rss_mb) "
                                            "for the VS-vs-Eduction time+memory head-to-head")
    ap.add_argument("--eduction-bin", help="path to Eduction binary (best-effort; usually not run here)")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--markdown-out")
    ap.add_argument("--apply-safe", action="store_true")
    ap.add_argument("--apply-risky", action="store_true")
    ap.add_argument("--fail-on-parity-regression", action="store_true")
    ap.add_argument("--no-leftmost-longest", action="store_true",
                    help="disable collapsing VS all-end-positions to Eduction's non-overlapping model")
    ap.add_argument("--no-char-offsets", action="store_true",
                    help="disable byte->char offset mapping on multibyte corpora (compare raw byte offsets)")
    args = ap.parse_args()

    if not Path(args.grammar).exists(): die(f"grammar not found: {args.grammar}")
    if not Path(args.corpus).exists(): die(f"corpus not found: {args.corpus}")

    fmt = detect_format(args.grammar, args.format)
    rep = {"grammar": args.grammar, "corpus": args.corpus, "format": fmt, "perf_guide": PERF_GUIDE}

    norm_path, finding = normalize_grammar(args.grammar, fmt)
    if norm_path is None:
        # No usable grammar (unsupported format / unconvertible XML) — report cleanly, do not benchmark.
        rep["unsupported"] = finding
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(rep, indent=2))
        warn(f"{fmt}: {finding['reason']} — {finding['detail']}")
        info(f"report written: {args.out}")
        # Not a whole-PR failure: unsupported format is reported cleanly.
        return
    if finding is not None:
        # A usable path WITH an info finding (e.g. XML was auto-converted) — record and continue.
        rep["conversion"] = finding
        if finding.get("type") == "converted":
            info(f"xml converted -> {finding['converted_hsg']}")

    exe = ensure_bench_exe(args)

    for _ in range(max(0, args.warmup)):
        run_bench_once(exe, norm_path, args.corpus)
    runs = [run_bench_once(exe, norm_path, args.corpus) for _ in range(max(1, args.repeat))]
    if any("error" in r for r in runs):
        rep["benchmark_error"] = next(r for r in runs if "error" in r)
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(rep, indent=2))
        die(f"benchmark failed: {rep['benchmark_error']}", 1)
    bench = aggregate(runs)
    rep["benchmark"] = bench

    parity = compute_parity(args, bench["detections"])
    rep["parity"] = parity
    if parity.get("reason") == "eduction_csv_schema_error":
        warn(f"Eduction CSV schema error: {parity.get('detail')} — parity reported UNAVAILABLE (not a false 0)")

    # VS-vs-Eduction time+memory head-to-head (the 'optimize' goal: faster + less memory than Eduction).
    if args.eduction_perf:
        if Path(args.eduction_perf).exists():
            rep["efficiency"] = head_to_head(bench, load_perf_csv(args.eduction_perf))
            h = rep["efficiency"]
            if h.get("available"):
                mem = (f", memory {h['vs_rss_mb']} vs {h['eduction_rss_mb']} MB "
                       f"({'VS lighter' if h.get('vs_less_memory') else 'VS heavier'})") if "vs_rss_mb" in h else ""
                info(f"efficiency: VS {'faster' if h['vs_faster'] else 'slower'} "
                     f"{h.get('time_speedup_x')}x ({h['vs_wall_ms']} vs {h['eduction_wall_ms']} ms){mem}")
        else:
            rep["efficiency"] = {"available": False, "reason": "eduction_perf_not_found"}

    pats, partial, note = extract_patterns(norm_path, fmt)
    findings, note = static_findings(pats, note)
    if fmt == "spec":
        rx_finding = spec_regex_component_finding(norm_path)
        if rx_finding is not None:
            findings.append(rx_finding)
            warn(f"spec: {rx_finding['kind']} — {rx_finding['detail']}")
    rep["static_findings"] = findings
    rep["static_partial"] = partial
    if note:
        rep["static_note"] = note

    if args.apply_safe or args.apply_risky:
        rep["apply"] = apply_loop(norm_path, fmt, exe, args.corpus, args, bench, parity)

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(rep, indent=2))
    info(f"report written: {args.out}")
    if args.markdown_out:
        Path(args.markdown_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.markdown_out).write_text(to_markdown(rep))
        info(f"markdown written: {args.markdown_out}")

    if args.fail_on_parity_regression:
        if parity and parity.get("available"):
            if not parity["parity"]:
                die("parity regression: VS-vs-Eduction baseline mismatch "
                    f"(FP {parity['false_positives']}, FN {parity['false_negatives']})", 1)
        else:
            warn("--fail-on-parity-regression set but Eduction parity unavailable; NOT failing on missing data.")


if __name__ == "__main__":
    main()
