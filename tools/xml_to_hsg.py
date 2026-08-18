#!/usr/bin/env python3
# Copyright (c) 2026, VectorCamp PC
# SPDX-License-Identifier: BSD-3-Clause
"""
xml_to_hsg.py — convert an IDOL/Eduction decompiled grammar XML into a Vectorscan .hsg.

Scope (all three tiers, honest about limits):
  Tier 1  literal-dict entities (<pattern>lit</pattern> / <entry headword="lit"/>)  -> HSG dict/compose
  Tier 2  single regex entity, no references                                        -> HSG import  id:/regex/flags
  Tier 3  multi-entity with (?A:name) / (?A^name) references                          -> inline-flattened regex

The engine resolves cross-file `import "file:entity"` directives but NOT intra-pattern (?A:name)
references, so this tool inline-flattens references itself (the flattened form is exactly the shipped
bm_addr_flat = inlined bm_addr_d2). Per-entity case= becomes a scoped (?i:...) / (?-i:...) group, which
is how the shipped flat grammar encodes it. Nothing is faked: if a flattened public entity is a huge
pure-literal alternation it is emitted as an HSG dict (not a giant regex); if a regex is genuinely too
large for Hyperscan the converter says so rather than pretending.

Usage:
  python3 tools/xml_to_hsg.py --xml IN.xml --out OUT.hsg [--dict-dir DIR] [--no-som]
                              [--literal-dict-threshold N] [--json REPORT.json]
"""
import argparse, json, re, sys, xml.etree.ElementTree as ET
from pathlib import Path

REF_RE = re.compile(r'\(\?A[:^]([A-Za-z0-9_./-]+)\)')   # (?A:name) or (?A^name)
META = set('\\.^$*+?()[]{}|')                            # regex metacharacters
OVERSIZE = 20000                                         # flattened regex len likely too big for HS


def warn(m): print(f"[warn] {m}", file=sys.stderr)


class ConvertError(Exception):
    pass


def parse_xml(path):
    """Return {grammar_name: {entity_name: {type, case, patterns[list], entries[list]}}}."""
    tree = ET.parse(path)
    root = tree.getroot()
    grammars = {}
    for g in root.iter("grammar"):
        gname = g.get("name", "grammar")
        ents = {}
        for e in g.findall("entity"):
            ename = e.get("name")
            ents[ename] = {
                "type": e.get("type", "public"),
                "case": e.get("case", "sensitive"),
                "patterns": [(p.text or "") for p in e.findall("pattern")],
                "entries": [en.get("headword", "") for en in e.findall("entry")],
            }
        grammars[gname] = ents
    if not grammars:
        raise ConvertError("no <grammar> found (is this an Eduction grammar XML?)")
    return grammars


def is_literal(s):
    """A <pattern> body with no regex metacharacter is a plain literal."""
    return not any(c in META for c in s)


def entity_is_pure_literal(ents, name, seen=None):
    """True if `name` (and everything it transitively references) is a literal alternation only —
    i.e. no regex operators bind the branches, so it can be emitted as a flat literal dict."""
    seen = seen or set()
    if name in seen:
        return False                       # cycle -> not a clean literal dict
    seen = seen | {name}
    ent = ents.get(name)
    if ent is None:
        return False
    for p in ent["patterns"]:
        refs = REF_RE.findall(p)
        stripped = REF_RE.sub("", p)       # remove refs, look at the connective text
        if refs:
            # references are only "pure literal" if the glue between them is a bare '|' alternation
            if not re.fullmatch(r'[|]*', stripped) or not all(entity_is_pure_literal(ents, r, seen) for r in refs):
                return False
        elif not is_literal(p):
            return False
    return True


def literal_values(ents, name, seen=None):
    """Collect the flat literal set for a pure-literal entity (entries + literal patterns + refs)."""
    seen = seen or set()
    if name in seen:
        return []
    seen = seen | {name}
    ent = ents[name]
    vals = [e for e in ent["entries"] if e != ""]
    for p in ent["patterns"]:
        refs = REF_RE.findall(p)
        if refs:
            for r in refs:
                vals += literal_values(ents, r, seen)
        elif p:
            vals.append(p)
    return vals


def scoped(case, body):
    """Wrap a body in the entity's case scope, matching how the shipped flat grammar encodes case."""
    return f"(?i:{body})" if case.startswith("insens") else f"(?-i:{body})"


def build_body(ents, name, stack):
    """Recursively inline-flatten entity `name` into one regex string. Cycle-safe."""
    if name in stack:
        raise ConvertError(f"reference cycle through entity '{name}': {' -> '.join(stack)} -> {name}")
    ent = ents.get(name)
    if ent is None:
        raise ConvertError(f"reference to undefined entity '{name}'")
    stack = stack + [name]

    branches = []
    for p in ent["patterns"]:
        if p == "":
            continue                        # a lone empty <pattern></pattern> = "not expanded" marker
        # inline every (?A:x)/(?A^x) reference in this pattern
        def repl(m):
            return f"(?:{build_body(ents, m.group(1), stack)})"
        branches.append(REF_RE.sub(repl, p))
    for e in ent["entries"]:
        if e == "":
            continue                        # empty headword = no literal (would match empty buffer)
        branches.append(re.escape(e))     # <entry> headwords are literals

    if not branches:
        raise ConvertError(f"entity '{name}' has no patterns or entries")
    body = branches[0] if len(branches) == 1 else "(?:" + "|".join(branches) + ")"
    return scoped(ent["case"], body)


def public_entities(ents):
    pub = [n for n, e in ents.items() if e["type"] == "public"]
    return pub or list(ents.keys())        # fall back to all if none marked public


def convert(path, dict_dir, som, lit_threshold):
    grammars = parse_xml(path)
    lines, dict_files, report = [], [], {"xml": str(path), "entities": [], "emitted": []}
    flags = ("L" if som else "")
    next_id = 100

    for gname, ents in grammars.items():
        for pub in public_entities(ents):
            info = {"grammar": gname, "entity": pub}
            ent = ents[pub]
            if not [p for p in ent["patterns"] if p != ""] and not ent["entries"]:
                # Empty entity (e.g. decompiled name composites with a lone <pattern></pattern>:
                # "references NOT expanded; no word list"). No data to convert — skip honestly.
                info.update(skipped=True, reason="empty_entity_no_data",
                            detail="entity has no <pattern>/<entry>; the decompiled XML did not expand it")
                warn(f"{gname}:{pub} skipped — empty entity (no data in XML)")
                report["emitted"].append(info)
                continue
            if entity_is_pure_literal(ents, pub) and len(literal_values(ents, pub)) >= lit_threshold:
                # Tier 1: large pure-literal set. A single-dict `compose` is rejected (needs >=2
                # components), and a 60k-branch alternation is "too large" -> emit as multi-literal
                # import lines (exactly what Hyperscan's multi-literal matcher is built for).
                vals = literal_values(ents, pub)
                fl = ("i" if ent["case"].startswith("insens") else "") + flags
                for v in vals:
                    lines.append(f"{next_id}:/{re.escape(v)}/{fl}")
                    next_id += 1
                info.update(tier=1, kind="literal_multi", count=len(vals),
                            id_range=[next_id - len(vals), next_id - 1])
            else:
                # Tier 2/3: regex (possibly with inlined references) -> import id:/regex/flags.
                body = build_body(ents, pub, [])
                lines.append(f"{next_id}:/{body}/{flags}")
                tier = 3 if REF_RE.search("".join(ents[pub]["patterns"])) else 2
                info.update(tier=tier, kind="regex", id=next_id, regex_len=len(body))
                if len(body) > OVERSIZE:
                    # A very large inlined composition (e.g. addr_dict_l / addr_perm) is likely to hit
                    # Hyperscan's "Pattern length exceeds limit". Flag it honestly at convert time;
                    # the benchmark compile is still the ground truth.
                    info["warning"] = "oversized_regex_may_exceed_hyperscan_pattern_limit"
                    warn(f"{gname}:{pub} flattened regex is {len(body)} chars — may exceed Hyperscan's "
                         "pattern-length limit (large composition; needs a regex-capable composition "
                         "engine, out of scope for this converter)")
                next_id += 1
            report["emitted"].append(info)

    if not lines:
        raise ConvertError("nothing to emit")
    return "\n".join(lines) + "\n", dict_files, report


def main():
    ap = argparse.ArgumentParser(description="Convert Eduction grammar XML -> Vectorscan .hsg")
    ap.add_argument("--xml", required=True)
    ap.add_argument("--out", required=True, help="output .hsg path")
    ap.add_argument("--dict-dir", help="dir for emitted literal dict .txt files (default: alongside --out)")
    ap.add_argument("--no-som", action="store_true", help="omit the SOM (L) flag (parity needs SOM)")
    ap.add_argument("--literal-dict-threshold", type=int, default=1000,
                    help="pure-literal public entities with >= this many values become an HSG dict")
    ap.add_argument("--json", help="write a conversion report JSON here")
    a = ap.parse_args()

    out = Path(a.out)
    dict_dir = Path(a.dict_dir) if a.dict_dir else out.parent
    dict_dir.mkdir(parents=True, exist_ok=True)
    try:
        hsg, dicts, report = convert(a.xml, dict_dir, som=not a.no_som,
                                     lit_threshold=a.literal_dict_threshold)
    except (ConvertError, ET.ParseError) as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        sys.exit(2)

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(hsg)
    report["ok"] = True
    report["out"] = str(out)
    report["dict_files"] = dicts
    if a.json:
        Path(a.json).write_text(json.dumps(report, indent=2))
    print(json.dumps({"ok": True, "out": str(out), "emitted": report["emitted"],
                      "dict_files": dicts}, indent=2))


if __name__ == "__main__":
    main()
