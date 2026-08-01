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

import "other.hsg"              # import a whole grammar file
import "other.hsg:firstname"    # import only one named entity from that file

300:/[0-9]{3}-[0-9]{4}/L        # <id>:/<regex>/<flags>  (flags are Vectorscan flag letters)
```

- **`import "file.hsg"`** — pulls in every pattern of that file.
- **`import "file.hsg:entity"`** — pulls in only the named entity's patterns (selective import).
- **`entity <name>:`** — groups the following pattern lines under a named entity.
- **`<id>:/<regex>/<flags>`** — a pattern line. `<flags>` are the same letters Vectorscan already
  uses (`i`, `s`, `m`, `H`, `V`, `W`, `8`, `P`, `L`, `C`, `Q`).

Imports are resolved **recursively**, with **cycle detection** and **de-duplication**. A bad path, an
import cycle, or a reference to a missing entity produces a clean `hs_compile_error_t` (no crash).

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

Expected — 7 tests pass:

```
[ RUN      ] GrammarImport.EntityAddressed           # import "names.hsg:firstname": John matches, Smith does NOT
[ RUN      ] GrammarImport.WholeFileImport           # import "names.hsg": all entities match
[ RUN      ] GrammarImport.BadEntityIsCleanError     # import "names.hsg:nope" -> HS_COMPILER_ERROR
[ RUN      ] GrammarImport.MissingImportIsCleanError # import "missing.hsg"   -> HS_COMPILER_ERROR
[ RUN      ] GrammarImport.CycleDetected             # A -> B -> A            -> HS_COMPILER_ERROR, no hang
[ RUN      ] GrammarImport.MultiLevelChain           # L1 -> L2 -> L3 -> L4 all resolve
[ RUN      ] GrammarImport.FlagLetterSom             # /L = SOM_LEFTMOST is applied
[  PASSED  ] 7 tests.
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
