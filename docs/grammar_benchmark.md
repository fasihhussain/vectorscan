# Grammar benchmarking + optimization tooling

Benchmark a Vectorscan/Hyperscan grammar, optionally compare it to an Eduction baseline, surface
[Hyperscan performance-guide](https://intel.github.io/hyperscan/dev-reference/performance.html)
findings, and (conservatively) apply optimizations with before/after evidence.

## Components
| Path | Role |
|---|---|
| `tools/vs_grammar_bench_exec.cpp` → target `vs_grammar_bench` | C++ exec: compile + scan measurement, emits JSON (incl. a `detections` array). No optimization/Claude logic. |
| `tools/vs_grammar_bench.py` | Orchestrator: build-check, normalize, warmup+repeat, aggregate, Eduction parity, static findings, apply loop, JSON/Markdown report. |
| `.claude/skills/vectorscan-grammar-optimizer/SKILL.md` | Claude Skill that drives the workflow with guardrails. |
| `.claude/skills/vectorscan-grammar-optimizer/scripts/classify.py` | Classifies findings: `auto_apply_eligible` vs `review_required`. |

## Supported input formats
- **`.hsg`** — compiled through the public `hs_compile_multi()` + `HS_FLAG_GRAMMAR_REF` path.
- **`.spec`** — same public path (requires the compose/`.spec` support merged from the CFG 1-layer PR;
  present on `develop`). If run on a branch without it, `.spec` reports a structured
  `unsupported/pending_pr3` finding rather than faking support.
- **XML — best-effort.** This fork has **no general XML→HSG converter**. If one exists it is used;
  otherwise the report contains a structured **`converter_missing`** finding. XML is **not** faked, and
  benchmark-specific XML flatteners are **not** used as general converters. An unsupported format is a
  clean per-format finding, not a whole-run failure.

## Build-check
On startup, if the exec is missing the script runs (using `cmake --build`, not raw `make`):
```
# if build dir missing:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
# then:
cmake --build build --target vs_grammar_bench -j
```
Override with `--build-dir`, `--bench-exe`, `--target`.

## Example commands
```bash
# .hsg, no Eduction baseline
python3 tools/vs_grammar_bench.py \
  --grammar tools/testdata/tiny.hsg --corpus tools/testdata/tiny_corpus.txt \
  --out reports/tiny.json --markdown-out reports/tiny.md

# .spec with an existing Eduction CSV baseline
python3 tools/vs_grammar_bench.py \
  --grammar generated/broad_list_name_benchmark/composition/broad_list.spec \
  --corpus generated/broad_list_name_benchmark/input.txt \
  --eduction-output generated/broad_list_name_benchmark/composition/ed-broad_list-detections.csv \
  --build-dir build --out reports/broad_list_bench.json --markdown-out reports/broad_list_bench.md
```
Note: the second example's paths live in the separate **Testing workspace**, not this fork, and it
requires `.spec`/compose support (present on `develop` post-PR-#3). Use `tools/testdata/` for
self-contained runs inside the fork.

## Eduction comparison modes
- **`--eduction-output <csv>`** — compare VS detections to an existing Eduction CSV
  (`file,entity,text,start_offset,end_offset`). Parity is by span, after the standard normalization
  (drop pure-space tokens, strip one trailing space).
- **`--eduction-bin <path>`** — best-effort; live Eduction needs jar + environment setup, so it is
  usually reported `unavailable` rather than run.
- **Neither** — VS still benchmarks; parity is marked `unavailable` (no parity-safety claim).
- **`--fail-on-parity-regression`** — exits non-zero when parity is available AND mismatches / worsens;
  when parity is unavailable it warns and does **not** fail solely on missing data.

## Static-analysis findings (perf guide)
Best-effort, extracted per format (compose/`.spec` grammars are literal-based, so few regex-level
checks apply — reported honestly). Findings are **potential optimization opportunities, not proven
bugs**: leading `.*`/`.+`, no required literal, large bounded repeats `{100,1000}`, nested quantifiers,
case-insensitive broad tokens, `SOM_LEFTMOST` usage, and possible SINGLEMATCH/DOTALL/anchoring
opportunities. **Large alternation is flagged only as a compile-time/DB-size/memory investigation
item — never "rewrite this."**

## Safe vs risky optimization categories
- **auto_apply_eligible (safe):** output-preserving only — whitespace/comment normalization,
  duplicate-identical cleanup, no-op metadata cleanup. Cannot change compile/scan/detections.
- **review_required (risky):** removing SOM, adding SINGLEMATCH/anchors/DOTALL, rewriting leading `.*`,
  changing bounded repeats / case sensitivity / broad tokens / required literals, rewriting
  alternation. **SOM removal is always review_required.**

## Auto-apply + rollback behavior
- `--apply-safe` applies only safe (output-preserving) changes; `--apply-risky` is required for anything
  risky.
- Each applied change: **backup → apply one → diff → re-benchmark → re-check parity → keep only if
  performance is not worse and parity does not regress → else roll back → record** (file, before/after,
  reason, benchmark before/after, parity before/after, kept/reverted).
- **Current limitation (honest):** this first iteration auto-applies only the conservative
  output-preserving change (whitespace normalization, verified no-op). Risky regex/flag optimizations
  are surfaced + classified as `review_required`/suggested-only, **not** auto-rewritten. Full per-change
  auto-rewriting is a documented follow-up; nothing is faked.

## How the Claude Skill uses this
See `.claude/skills/vectorscan-grammar-optimizer/SKILL.md`: run the benchmark → read numbers → compare
to Eduction when available → classify findings → apply one safe change at a time with a shown diff →
re-benchmark → roll back on parity regression → report before/after. If parity is unavailable, it says
so and makes no parity-safety claim.

Perf guide: https://intel.github.io/hyperscan/dev-reference/performance.html
