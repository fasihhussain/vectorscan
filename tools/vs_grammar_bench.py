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
        return None, {
            "type": "unsupported", "format": "xml", "reason": "converter_missing",
            "detail": ("No general XML->HSG converter exists in this fork, so XML input is not "
                       "supported end-to-end. Convert to .hsg/.spec, or add a converter. "
                       "(Benchmark-specific XML flatteners are NOT used here — they are not general.)"),
        }
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
    return {
        "runs": len(vs),
        "compile_ms": stat("compile_ms"),
        "scan_ms": stat("scan_ms"),
        "throughput_mb_s": stat("throughput_mb_s"),
        "matches": first["matches"],
        "corpus_bytes": first["corpus_bytes"],
        "detections": first.get("detections", []),
        "detections_truncated": first.get("detections_truncated", False),
    }


# ---------------------------------------------------------------- Eduction parity
def _norm_ed_rows(rows):
    """Drop pure-space tokens and strip one trailing space (same normalization as prior deliverables)."""
    out = []
    for (ent, s, e, text) in rows:
        if text.strip() == "" and text != "":
            continue  # pure-space artifact token
        if text.endswith(" ") and e > s:
            e -= 1
        out.append((s, e))
    return set(out)


def load_eduction_csv(path):
    rows = []
    with open(path, newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                rows.append((row.get("entity", "").split("/")[-1],
                             int(row["start_offset"]), int(row["end_offset"]), row.get("text", "")))
            except (KeyError, ValueError):
                continue
    return rows


def parity_from_csv(vs_dets, ed_rows):
    vs = set((d["from"], d["to"]) for d in vs_dets)
    ed = _norm_ed_rows(ed_rows)
    fp = sorted(vs - ed)   # in VS, not Eduction
    fn = sorted(ed - vs)   # in Eduction, not VS
    return {
        "available": True, "vs_spans": len(vs), "ed_spans": len(ed),
        "false_positives": len(fp), "false_negatives": len(fn),
        "span_matched": len(vs & ed),
        "parity": (len(fp) == 0 and len(fn) == 0),
        "fp_examples": fp[:10], "fn_examples": fn[:10],
    }


def run_eduction_bin(edbin, corpus):
    warn(f"--eduction-bin given ({edbin}) but live Eduction execution is environment-specific "
         "(jar + quarantine setup); not run here. Provide --eduction-output CSV for parity.")
    return {"available": False, "reason": "eduction_bin_not_run"}


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
            after_par = parity_from_csv(after["detections"], load_eduction_csv(args.eduction_output))
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
    ap.add_argument("--eduction-output", help="existing Eduction detections CSV")
    ap.add_argument("--eduction-bin", help="path to Eduction binary (best-effort; usually not run here)")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--markdown-out")
    ap.add_argument("--apply-safe", action="store_true")
    ap.add_argument("--apply-risky", action="store_true")
    ap.add_argument("--fail-on-parity-regression", action="store_true")
    args = ap.parse_args()

    if not Path(args.grammar).exists(): die(f"grammar not found: {args.grammar}")
    if not Path(args.corpus).exists(): die(f"corpus not found: {args.corpus}")

    fmt = detect_format(args.grammar, args.format)
    rep = {"grammar": args.grammar, "corpus": args.corpus, "format": fmt, "perf_guide": PERF_GUIDE}

    norm_path, finding = normalize_grammar(args.grammar, fmt)
    if finding is not None:
        rep["unsupported"] = finding
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(rep, indent=2))
        warn(f"{fmt}: {finding['reason']} — {finding['detail']}")
        info(f"report written: {args.out}")
        # Not a whole-PR failure: unsupported format is reported cleanly.
        return

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

    parity = None
    if args.eduction_output:
        if Path(args.eduction_output).exists():
            parity = parity_from_csv(bench["detections"], load_eduction_csv(args.eduction_output))
        else:
            parity = {"available": False, "reason": "eduction_output_not_found"}
    elif args.eduction_bin:
        parity = run_eduction_bin(args.eduction_bin, args.corpus)
    else:
        parity = {"available": False, "reason": "no_eduction_data"}
    rep["parity"] = parity

    pats, partial, note = extract_patterns(norm_path, fmt)
    findings, note = static_findings(pats, note)
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
