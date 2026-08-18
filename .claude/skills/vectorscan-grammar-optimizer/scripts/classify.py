#!/usr/bin/env python3
# Copyright (c) 2026, VectorCamp PC
# SPDX-License-Identifier: BSD-3-Clause
"""
classify.py — read a vs_grammar_bench.py JSON report and classify its static findings into:
  1. auto_apply_eligible  — VERY conservative, output-preserving only
  2. review_required      — anything that can change matches / spans / flags / parity

Policy: only output-preserving cleanups (whitespace/comment/duplicate-identical/no-op metadata) are
auto-eligible. Every regex/flag change is review_required. SOM removal is ALWAYS review_required
(removing HS_FLAG_SOM_LEFTMOST changes start offsets/spans and can affect CFG/composition + parity).
"""
import argparse, json, sys

# Output-preserving cleanups only — safe to auto-apply.
AUTO_KINDS = {
    "whitespace_normalization", "comment_normalization",
    "duplicate_identical_pattern", "noop_metadata_cleanup",
}
# Everything below can change match count / spans / matched text / FP-FN / CFG behavior / parity.
REVIEW_KINDS = {
    "leading_dotstar", "leading_dotplus", "no_required_literal", "large_bounded_repeat",
    "nested_quantifier", "case_insensitive", "large_alternation", "som_leftmost",
    "remove_som", "add_singlematch", "add_dotall", "add_anchor",
    "rewrite_alternation", "change_bounded_repeat", "change_literal", "change_case_sensitivity",
}


def classify(rep):
    out = {"auto_apply_eligible": [], "review_required": []}
    for f in rep.get("static_findings", []):
        kind = f.get("kind", "")
        entry = dict(f)
        if kind in AUTO_KINDS:
            entry["classification"] = "auto_apply_eligible"
            entry["reason"] = "output-preserving cleanup; cannot change compile/scan/detections"
            out["auto_apply_eligible"].append(entry)
        else:
            entry["classification"] = "review_required"
            reason = ("can alter match count, start/end spans, matched text, false positives/negatives, "
                      "CFG composition behavior, or Eduction parity")
            if kind == "som_leftmost":
                reason += " — SOM removal changes start offsets/spans; never auto-apply"
            if kind == "large_alternation":
                reason += " — treat as a measurement concern (compile/db-size/memory), do NOT auto-rewrite"
            entry["reason"] = reason
            out["review_required"].append(entry)
    out["summary"] = {"auto_apply_eligible": len(out["auto_apply_eligible"]),
                      "review_required": len(out["review_required"])}
    out["policy"] = ("Only output-preserving cleanups auto-apply. All regex/flag changes "
                     "(including SOM removal) are review_required and must be benchmark+parity gated.")
    return out


def main():
    ap = argparse.ArgumentParser(description="Classify benchmark static findings (auto_apply_eligible vs review_required).")
    ap.add_argument("report", help="JSON report produced by tools/vs_grammar_bench.py")
    ap.add_argument("--out", help="write classification JSON here (default: stdout)")
    a = ap.parse_args()
    try:
        rep = json.load(open(a.report))
    except (OSError, json.JSONDecodeError) as e:
        print(f"[error] cannot read report {a.report}: {e}", file=sys.stderr); sys.exit(2)
    res = classify(rep)
    s = json.dumps(res, indent=2)
    if a.out:
        open(a.out, "w").write(s); print(f"[info] classification written: {a.out}", file=sys.stderr)
    else:
        print(s)


if __name__ == "__main__":
    main()
