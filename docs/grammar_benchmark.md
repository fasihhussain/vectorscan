# Grammar benchmarking + optimization tooling

Benchmark a Vectorscan/Hyperscan grammar, optionally compare it to an Eduction baseline, surface
[Hyperscan performance-guide](https://intel.github.io/hyperscan/dev-reference/performance.html)
findings, and (conservatively) apply optimizations with before/after evidence.

## Components
| Path | Role |
|---|---|
| `tools/vs_grammar_bench_exec.cpp` → target `vs_grammar_bench` | C++ exec: compile + scan measurement, emits JSON (incl. a `detections` array). No optimization/Claude logic. |
| `tools/vs_grammar_bench.py` | Orchestrator: build-check, normalize (incl. XML auto-convert), warmup+repeat, aggregate, Eduction parity (with match-semantics normalization), static findings, apply loop, JSON/Markdown report. |
| `tools/xml_to_hsg.py` | Eduction/IDOL grammar XML → `.hsg` converter (literal dicts, regex, inline-flattened `(?A:name)` composition). Honest about empty/oversized/decompiled-composite limits. |
| `.claude/skills/vectorscan-grammar-optimizer/SKILL.md` | Claude Skill that drives the workflow with guardrails. |
| `.claude/skills/vectorscan-grammar-optimizer/scripts/classify.py` | Classifies findings: `auto_apply_eligible` vs `review_required`. |

## Supported input formats
- **`.hsg`** — compiled through the public `hs_compile_multi()` + `HS_FLAG_GRAMMAR_REF` path.
- **`.spec`** — same public path (requires the compose/`.spec` support merged from the CFG 1-layer PR;
  present on `develop`). If run on a branch without it, `.spec` reports a structured
  `unsupported/pending_pr3` finding rather than faking support.
  - **Known limitation — literal-component only.** The exec compiles `.spec` `P`-lines as **literals**
    (`hs_compile_lit_multi`, no regex). Literal-component specs (broad_list / name style) are fully
    supported. **Regex-component specs** (e.g. the addr benchmark specs whose `P`-lines are patterns like
    `P<TAB>street<TAB>(?i:...)`) are matched **literally, not as regexes** — so they are *not* semantically
    equivalent to the Eduction grammar. When ≥10% of `P`-lines look like regexes the report emits a
    `regex_component_spec_unsupported` **limitation** finding; do **not** claim parity for such specs. This
    would need engine support (regex `P`-line compilation) and is out of scope for the tooling — it is
    documented here rather than faked.
- **XML — auto-converted** via `tools/xml_to_hsg.py` (Eduction/IDOL decompiled grammar XML). The
  benchmark calls the converter automatically; the report records a `conversion` block listing each
  emitted entity + tier. Three tiers, all faithful, none faked:
  - **Tier 1 — literal dicts** (`<pattern>lit</pattern>` / `<entry headword="lit"/>`) → multi-literal
    `id:/lit/` patterns (census + name leaf dictionaries).
  - **Tier 2 — single regex** entity → import `id:/regex/`.
  - **Tier 3 — composition** (`(?A:name)` / `(?A^name)` references) → inline-flattened into one regex
    (the engine resolves cross-file `import` directives but not intra-pattern refs, so the converter
    inlines them; the flattened form equals the shipped `bm_addr_flat`).
  - **Honest limits:** empty entities (decompiled composites with no data) are **skipped** with a
    reason; a flattened composition that exceeds Hyperscan's pattern-length limit is reported as a
    compile error (the large `addr_dict_l/m`, `addr_full`, `addr_perm_*` grammars — they need a
    regex-capable composition engine, out of scope). Name **composite** grammars can't be reproduced
    from the decompiled XML because the XML carries only the leaf dicts + connector fragments, not the
    composition — reported, not faked.

## Timing phases (compile / regex / Earley)
The report breaks VS processing into three phases (median ms), so you can see where time goes:
- **VS compilation** (`compile_ms`) — `hs_compile_multi` building the pattern database.
- **regex processing** (`regex_ms`) — the base literal/regex scan (`hs_scan`).
- **Earley/composition** (`earley_ms`) — the CFG **position-join** that assembles composites. (We have
  no literal Earley parser; this is the moral equivalent — the composition phase.) Only compose
  grammars (`.spec` / `.hsg compose`, e.g. broad_list) have it; **plain/flat-regex grammars report
  `earley_ms = 0` and `phase_split = false`** (their whole scan is regex).

How it's measured (scalable, containment-preserving): the CFG engine records the two scan-phase
timings **only when `VS_CFG_TIMING_OUT` is set** (env-gated → zero overhead and zero behaviour change
otherwise) to a keyval side-file; the exec sets that env, reads the file back, and folds `regex_ms` /
`earley_ms` into its JSON. No cfg symbol is exported. `scan_ms` remains the authoritative total wall
(it also covers dispatch setup + result emission, so `scan_ms ≥ regex_ms + earley_ms`).

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

## VS-vs-Eduction time + memory head-to-head (`--eduction-perf`)
The core "is Vectorscan faster and lighter than Eduction?" comparison. Give the tool an Eduction
**performance CSV** (`run_number,wall_ms,cpu_ms,rss_mb,...`); the report's `efficiency` block reports:
- **time** — VS wall (`compile_ms + scan_ms`, this tool's live run) vs Eduction `wall_ms` → speedup ×.
- **memory** — VS **peak RSS** (`getrusage`, whole process) vs Eduction `rss_mb` → ratio.

Both engines' medians exclude the warmup run 0. **Honest caveats:**
- VS wall here **includes compile** (a one-time cost), so the speedup is *conservative* vs a scan-only
  comparison.
- VS memory is **whole-process peak RSS**, which includes the one-time `hs_compile` transient. On small
  literal-dict/sequence compositions that transient can push VS peak **above** Eduction even though VS is
  far faster; the project's dedicated RSS pipeline mitigates it (e.g. dropping `HS_FLAG_UCP`). The tool
  reports what it measures and states when VS is heavier — it never hides a loss.
- On the 15-grammar mentor suite (9 runnable): VS is **faster on all 9** (~7×–1165×) and **lighter on
  6/9**; heavier on 3/9 (`addr_dict_s`, `money_seq`, `money_seq_nocomp`) under peak-RSS accounting.

## Eduction comparison modes
- **`--eduction-output <csv>`** — compare VS detections to an existing Eduction CSV
  (`file,entity,text,start_offset,end_offset`). Column names are **normalized** (lowercased, trimmed,
  spaces→underscores), so **both** `start_offset`/`end_offset` **and** `start offset`/`end offset` headers
  are accepted (real Eduction bench CSVs use the spaced form). If a CSV has rows but **no** recognizable
  start/end columns, parity is reported **unavailable** with an `eduction_csv_schema_error` reason — it is
  never a silent `0` detections / spurious `parity=True`. A genuinely empty file is honestly reported as
  0 rows.
  - **Parity normalizations** (each recorded in the report's `parity.normalization`, all off-switchable,
    none invent matches):
    - **corpus-file filter** — an Eduction CSV often concatenates detections for several corpus files
      (e.g. `a3__dense` + `a3__sparse`); only rows whose `file` column matches the scanned corpus are
      compared, so the other file's rows aren't counted as spurious FNs.
    - **`vs_trailing_space`** (when corpus is available) — strips one trailing space from VS spans by
      peeking at corpus bytes, restoring symmetry with the Eduction side (whose text column lets it drop
      one trailing space). The exec emits only `from/to` (no text), so VS can't self-normalize otherwise.
    - **`leftmost_longest`** (default; `--no-leftmost-longest` to disable) — Hyperscan reports *every*
      end position for a match; this collapses them to Eduction's non-overlapping leftmost-longest model.
    - **`char_offsets`** (auto on multibyte corpora; `--no-char-offsets` to disable) — maps VS **byte**
      offsets to **character** offsets, since Eduction reports character offsets (`Ö` = 1 char / 2 bytes).
  - Plus the standard span normalization (drop pure-space tokens, strip one trailing space).
- **`--eduction-bin <path>`** — best-effort; live Eduction needs jar + environment setup, so it is
  usually reported `unavailable` rather than run.
- **Neither** — VS still benchmarks; parity is marked `unavailable` (no parity-safety claim).
- **`--fail-on-parity-regression`** — exits non-zero when parity is available AND mismatches / worsens;
  when parity is unavailable it warns and does **not** fail solely on missing data.

## Static-analysis findings (perf guide)
Best-effort, extracted per format (compose/`.spec` grammars are literal-based, so few regex-level
checks apply — reported honestly). For **XML input**, analysis runs on the **converted `.hsg`** that is
actually benchmarked (the report records `static_analysis_format` / `static_analysis_target`), so an
XML grammar surfaces the same findings (e.g. `som_leftmost`, `large_alternation`) as its converted HSG. Findings are **potential optimization opportunities, not proven
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
