---
name: vectorscan-grammar-optimizer
description: Use when optimizing Vectorscan/Hyperscan grammars with benchmark evidence, Eduction parity comparison, and Hyperscan performance guidance. Benchmarks a .hsg/.spec grammar (XML best-effort), compares vs an Eduction baseline, surfaces performance-guide findings, and applies only conservative, verified changes.
---

# Vectorscan grammar optimizer

Optimize a Vectorscan/Hyperscan grammar **with evidence**: benchmark first, compare to Eduction when
available, and only change what a measurement + parity check justifies. Never claim an improvement
without before/after numbers.

Reference: Hyperscan performance guide — https://intel.github.io/hyperscan/dev-reference/performance.html

## Tools
- `tools/vs_grammar_bench.py` — orchestrator (build-check, normalize, benchmark, parity, static findings, apply loop).
- `tools/xml_to_hsg.py` — Eduction grammar XML → `.hsg` converter (auto-invoked on XML input).
- `tools/vs_grammar_bench_exec.cpp` → `vs_grammar_bench` — the C++ compile+scan measurement exec (JSON out).
- `.claude/skills/vectorscan-grammar-optimizer/scripts/classify.py` — classify findings safe vs risky.

## Workflow (follow in order)
1. **Run the benchmark:** `python3 tools/vs_grammar_bench.py --grammar <g> --corpus <c> --out report.json [--markdown-out report.md]`.
2. **Build if missing:** the script builds `vs_grammar_bench` automatically (`cmake --build build --target vs_grammar_bench -j`). Don't hand-build unless it fails.
3. **Normalize input:** `.hsg` and `.spec` run directly. **XML is auto-converted** by `tools/xml_to_hsg.py` (literal dicts → multi-literal; single regex → import; `(?A:name)` composition → inline-flattened regex); the report's `conversion` block lists emitted entities + tiers. Honest limits: empty decompiled composites are skipped, oversized flattened compositions hit Hyperscan's pattern-length limit (reported, not faked), and name composites aren't reproducible from decompiled XML. **`.spec` is literal-component only** — `P`-lines compile as literals, so a *regex-component* `.spec` (e.g. addr specs) emits a `regex_component_spec_unsupported` **limitation** finding; report it and do not claim parity for such specs.
4. **Read the VS numbers** (compile/scan/throughput/**peak RSS**/matches) from the report.
4b. **Time + memory vs Eduction:** when `--eduction-perf <csv>` is given, the report's `efficiency` block states whether VS is faster (speedup ×) and lighter (RSS ratio) than Eduction. VS wall includes compile (conservative); VS memory is whole-process peak RSS (includes the one-time `hs_compile` transient, so report honestly when VS is heavier — never hide it).
5. **Compare vs Eduction** when `--eduction-output <csv>` (or `--eduction-bin`) is given. CSV column names are normalized, so both `start_offset`/`end_offset` and `start offset`/`end offset` work. Parity applies documented, off-switchable match-semantics normalizations (recorded in `parity.normalization`): corpus-file filter, `leftmost_longest` (Hyperscan all-ends → Eduction non-overlapping), and `char_offsets` (byte→char on multibyte). If a CSV has rows but no recognizable start/end columns, parity is `eduction_csv_schema_error` (unavailable) — never a silent `0`. If neither source is available, the report marks parity `unavailable` — **say so and do not claim parity safety**.
6. **Read static findings** (perf-guide heuristics).
7. **Classify** with `classify.py report.json`: `auto_apply_eligible` vs `review_required`.
8. **Auto-apply only safe (output-preserving) changes** unless the user explicitly approves risky ones (`--apply-risky`).
9. **Apply one change at a time.**
10. **Show the exact diff** of every change.
11. **Re-run the benchmark after each change.**
12. **Roll back** the change if Eduction parity regresses (or matches change unexpectedly).
13. **Report before/after** numbers (benchmark + parity) for every change.
14. If Eduction parity is unavailable, **state that clearly** and avoid any parity-safety claim.

## Guardrails (do not violate)
- Never hide a grammar change — every edit is shown as a diff with a reason.
- Never claim a performance improvement without before/after benchmark numbers.
- Never reduce detection coverage unless the user explicitly approves.
- Never optimize just because a regex "looks slow" — require benchmark evidence.
- **SOM removal (`HS_FLAG_SOM_LEFTMOST`) is review_required, never auto** — it changes start offsets/spans and can affect CFG/composition + parity.
- **Do not hand-rewrite large alternations** just because they are large. Treat large alternation as a measurement concern: check compile time, database size, memory, and match behavior first (the perf guide warns against evidence-free regex rewriting).
- Risky rewrites (anchoring, DOTALL, SINGLEMATCH, bounded-repeat/literal/case changes, alternation rewrites) require `--apply-risky` **and** a re-verified parity check.
- If parity data is unavailable, do not use `--fail-on-parity-regression` as proof of safety.

## Current limitation (honest)
This first iteration's auto-apply performs only **conservative, output-preserving** changes
(whitespace/comment normalization, verified no-op via re-benchmark + parity). All regex/flag
optimizations are surfaced + classified as **review_required / suggested-only**, not auto-rewritten.
Full per-change auto-rewriting with benchmark+parity gating is a documented follow-up. Do not present
suggested-only findings as if they were applied.
