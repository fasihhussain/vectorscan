# Native grammar import (`.hsg`)

## What this adds

A native way for one Vectorscan grammar to **import** patterns from another, built into the library —
no external format, no XML, no third-party parser. It is exposed as a new compile flag on the existing
`hs_compile_multi()` API (no new public function):

```c
#define HS_FLAG_GRAMMAR_REF 2048
```

When an expression passed to `hs_compile_multi()` has this flag set, the expression string is treated
as the path to a Vectorscan **native grammar file** (`.hsg`). Before compilation, the file's `import`
directives are resolved into a plain pattern set, which is then compiled through the normal
multi-pattern path. Everything else about the call is unchanged.

## The `.hsg` format

```
# a grammar file may group patterns under named entities
entity firstname:
100:/John/
101:/Sarah/
entity lastname:
200:/Smith/

# import a whole grammar file
import "other.hsg"
# import only one named entity from that file
import "other.hsg:firstname"

# <id>:/<regex>/<flags>  (flags are Vectorscan flag letters)
300:/[0-9]{3}-[0-9]{4}/L
```

- **`import "file.hsg"`** — pulls in every pattern of that file.
- **`import "file.hsg:entity"`** — pulls in only the named entity's patterns (selective import).
- **`entity <name>:`** — groups the following pattern lines under a named entity.
- **`<id>:/<regex>/<flags>`** — a pattern line. `<flags>` are the same letters Vectorscan already
  uses (`i`, `s`, `m`, `H`, `V`, `W`, `8`, `P`, `L`, `C`, `Q`).
- **Comments** must be on their **own line** (a line whose first non-blank character is `#`). Inline
  comments after an `import`, `entity`, or pattern line are **not** supported in Phase 1 — such a line
  is treated as malformed and produces a clean compile error.

Imports are resolved **recursively**, with **cycle detection** and **de-duplication**. A bad path, an
import cycle, a reference to a missing entity, or malformed `.hsg` syntax produces a clean
`hs_compile_error_t` (no crash).

**De-duplication is at the import-target level, not the pattern level.** If the same file (or the same
`file:entity`) is imported more than once along the resolution graph, it is included only once — so a
diamond `A→B→C` plus `A→C` pulls `C` a single time. It does **not** deduplicate identical individual
patterns that arrive from *different* files; those are passed through to the compiler as-is (Vectorscan
handles duplicate patterns normally). This is the intended Phase-1 semantics.

## Where it lives

| File | Role |
|---|---|
| `src/grammar/grammar_parser.{h,cpp}` | parse one `.hsg` file (imports, entities, pattern lines, flag letters) |
| `src/grammar/import_resolver.{h,cpp}` | resolve imports recursively — whole-file or entity-addressed, cycle-safe, de-duped |
| `src/grammar/grammar_import.{h,cpp}` | expand `HS_FLAG_GRAMMAR_REF` inputs into a plain pattern set |
| `src/hs.cpp` | `hs_compile_multi()` calls the expander when the flag is present, then compiles as usual |

The module has **no dependency on XML, Boost, or any external grammar format** — it reads only
Vectorscan's own `.hsg`.

## Testing

Build and run the unit tests:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build -j unit-hyperscan
./build/bin/unit-hyperscan --gtest_filter='GrammarImport.*'
```

Expected — 10 tests pass:

```
[ RUN      ] GrammarImport.EntityAddressed           # import "names.hsg:firstname": John matches, Smith does NOT
[ RUN      ] GrammarImport.WholeFileImport           # import "names.hsg": all entities match
[ RUN      ] GrammarImport.BadEntityIsCleanError     # import "names.hsg:nope" -> HS_COMPILER_ERROR
[ RUN      ] GrammarImport.MissingImportIsCleanError # import "missing.hsg"   -> HS_COMPILER_ERROR
[ RUN      ] GrammarImport.CycleDetected             # A -> B -> A            -> HS_COMPILER_ERROR, no hang
[ RUN      ] GrammarImport.MultiLevelChain           # L1 -> L2 -> L3 -> L4 all resolve
[ RUN      ] GrammarImport.FlagLetterSom             # /L = SOM_LEFTMOST is applied
[ RUN      ] GrammarImport.MalformedLineIsError      # a line that is not comment/import/entity/pattern -> error
[ RUN      ] GrammarImport.GrammarRefRequiresHsg     # HS_FLAG_GRAMMAR_REF on a non-.hsg expression -> error
[ RUN      ] GrammarImport.ImportSyntaxIsStrict      # "importX ..." and 'import "..." junk' -> error
[  PASSED  ] 10 tests.
```

The test source is `unit/hyperscan/grammar_import.cpp`; it writes its own small `.hsg` fixtures to a
temp directory, so no external data files are needed.

## Scope

This is phase 1: **whole-file and entity-addressed import**, resolved recursively and safely.
Composition/combination of entities (an entity built from references to other entities) is intentionally
left for a follow-up.

**Input to the library is always `.hsg`.** Grammars that exist in another format (e.g. XML) are expected
to be converted to `.hsg` **outside** the library first; that conversion is a **separate concern, not
part of this PR** (a front-end tool to be added separately). Keeping it outside is deliberate — the
library core stays free of any external format.
