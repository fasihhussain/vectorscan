# `.hsg` grammar specification

Formal syntax of the Vectorscan native grammar file (`.hsg`). This documents the Phase-1 `.hsg`
syntax accepted by `src/grammar/grammar_parser.cpp` — the grammar of the **file format**, not the
future composition/chaining feature.

## 1. Overview

A `.hsg` file is a sequence of **lines**. Each non-blank line is one of:

- a **comment** (`# …`)
- an **import** (`import "…"`)
- an **entity header** (`entity <name>:`)
- a **pattern** (`<id>:/<regex>/<flags>`)

Each line is trimmed of leading/trailing whitespace before it is classified.

## 2. Formal grammar (EBNF)

```ebnf
hsg_file       = { line } ;

line           = blank
               | comment
               | import_stmt
               | entity_header
               | pattern ;

(* --- blank / comment --- *)
blank          = ws ;                                  (* empty after trimming *)
comment        = ws , "#" , { any_char } ;             (* whole-line comment only *)

(* --- import --- *)
import_stmt    = "import" , ws1 , '"' , import_target , '"' , ws ;
import_target  = file_path , [ ":" , entity_name ] ;   (* "f.hsg"  OR  "f.hsg:entity" *)

(* --- entity section header --- *)
entity_header  = "entity" , ws1 , entity_name , ":" ;

(* --- pattern --- *)
pattern        = id , ":" , "/" , regex , "/" , flags ;
id             = digit , { digit } ;
regex          = { any_char } ;                        (* text between the first '/' and the last '/' *)
flags          = { flag_letter } ;                     (* may be empty *)

(* --- lexical --- *)
flag_letter    = "i" | "s" | "m" | "H" | "V" | "W" | "8" | "P" | "L" | "C" | "Q" ;
entity_name    = name_char , { name_char } ;
name_char      = letter | digit | "_" | "/" | "." | "-" ;
file_path      = { path_char } ;                       (* any character except '"' *)
ws             = { " " | "\t" } ;                      (* optional whitespace *)
ws1            = ( " " | "\t" ) , ws ;                 (* at least one whitespace *)
digit          = "0" | "1" | … | "9" ;
letter         = "A" | … | "Z" | "a" | … | "z" ;
```

**Notes on the grammar above:**

- **Entity names** — `name_char` includes `/` on purpose: real entity names carry a locale-style path,
  e.g. `femalefirstname/engcn`. Phase-1 grammars should use identifiers matching `name_char`. (The
  current parser is more lenient — it accepts any non-empty trimmed text between `entity ` and the
  trailing `:` — but the set above is the intended, portable form.)
- **Entity selector split** — an import target is split into `path` and `entity` at the `.hsg:` suffix,
  not at a bare `:`. So a colon inside a path (e.g. a Windows drive letter, `C:\g\names.hsg`) is treated
  as part of the path, not as an entity selector.

## 3. Flag letters

The `<flags>` field uses the same letters Vectorscan already uses (identical to
`util/ExpressionParser.rl` / `readExpression`):

| Letter | HS flag |
|---|---|
| `i` | `HS_FLAG_CASELESS` |
| `s` | `HS_FLAG_DOTALL` |
| `m` | `HS_FLAG_MULTILINE` |
| `H` | `HS_FLAG_SINGLEMATCH` |
| `V` | `HS_FLAG_ALLOWEMPTY` |
| `W` | `HS_FLAG_UCP` |
| `8` | `HS_FLAG_UTF8` |
| `P` | `HS_FLAG_PREFILTER` |
| `L` | `HS_FLAG_SOM_LEFTMOST` |
| `C` | `HS_FLAG_COMBINATION` |
| `Q` | `HS_FLAG_QUIET` |

Any other character in the flags field makes the line invalid (a syntax error).

## 4. Semantics (beyond syntax)

- **`import "file.hsg"`** — includes every pattern of that file.
- **`import "file.hsg:entity"`** — includes only the patterns under that named entity.
- Imports are resolved **recursively**; the importing file's directory is used for **relative** paths,
  absolute paths are used as-is.
- **Cycle detection:** an import cycle (`A → B → A`) is rejected with a clean error, no infinite loop.
- **De-duplication:** the same `file` (or `file:entity`) reached more than once is included **once**
  (import-target level, not pattern level).
- A pattern belongs to the most recent `entity <name>:` header above it; patterns before any header
  belong to the file's default (unnamed) entity.
- Entity composition/combination is intentionally **not** part of Phase 1.

## 5. Well-formedness rules (what makes a line an error)

A non-blank, non-comment line that is not a valid `import`, `entity_header`, or `pattern` is a
**syntax error** and produces a clean `hs_compile_error_t` (`file.hsg:<line>: invalid .hsg syntax`).
In particular:

- `importwhatever "x.hsg"` — the `import` keyword must be followed by whitespace.
- `import "x.hsg" junk` — nothing but whitespace may follow the quoted path (no inline comment).
- `100:/abc/Z` — `Z` is not a valid flag letter.
- Inline comments after an `import`, `entity`, or pattern line are **not** supported; comments must be
  on their own line.

## 6. Example

```hsg
# person names grammar
entity firstname:
100:/John/
101:/Sarah/

entity lastname:
200:/Smith/

# reuse another grammar
import "common.hsg"
# reuse only one entity from it
import "common.hsg:dates"

300:/[0-9]{3}-[0-9]{4}/
```

## 7. Note on the `regex` field

`regex` is the text between the **first** `/` and the **last** `/` on the line; the `<flags>` are what
follows the last `/`. A regex may therefore contain `/` characters, as long as the trailing
`/<flags>` closes the line. (Phase 1 does not add extra escaping rules beyond this.)

**This section defines parser syntax only.** Phase 1 does not introduce a new regex language: after
import resolution, the regex strings are handed to the existing Vectorscan compile pipeline. A line
may therefore be syntactically valid `.hsg` yet still be rejected later by the normal Vectorscan
compiler if the regex itself is invalid or incompatible with the chosen flags/mode.

## 8. Compile API input

The library input is a `.hsg` file path passed to `hs_compile_multi()` with `HS_FLAG_GRAMMAR_REF`:

```c
const char *expressions[] = { "top.hsg" };
unsigned    flags[]       = { HS_FLAG_GRAMMAR_REF };
unsigned    ids[]         = { 0 };
hs_compile_multi(expressions, flags, ids, 1, HS_MODE_BLOCK, NULL, &db, &compile_err);
```

If `HS_FLAG_GRAMMAR_REF` is set, the expression must be a `.hsg` file path; a non-`.hsg` expression is
rejected with a clean compile error. XML is **not** accepted by the library — a grammar that exists in
XML must first be converted to `.hsg` by external tooling, outside the library.

## 9. Future extension points

`.hsg` is a Vectorscan-owned native file format, so future syntax can be added deliberately by updating
this specification and the parser. Phase 1 intentionally implements only import resolution, entity
selection, and pattern collection.

Features such as Eduction-style **scoring/confidence** could be added as **metadata**. For example, a
future per-pattern attribute block might look like:

```hsg
100:/John/i { score=90 }
101:/Jon/i  { score=60 }
```

A possible future grammar extension for that would be:

```ebnf
pattern     = id , ":" , "/" , regex , "/" , flags , [ ws1 , attr_block ] ;
attr_block  = "{" , ws , attr , { ws , "," , ws , attr } , ws , "}" ;
attr        = attr_key , "=" , attr_value ;
attr_key    = identifier ;
attr_value  = number | quoted_string | identifier ;
```

In that design, `score` would be metadata attached to a pattern id or entity. It could be used by a
future post-processing / chaining layer to rank matches, reduce false positives, or attach confidence
to extracted entities.

**Important — how this actually works:** the Phase-1 import resolver and the Vectorscan regex compiler
do **not** interpret `score`. Vectorscan compiles regex patterns and reports matches by id; any score
metadata would be **looked up after a match is reported**, by a separate layer built on top. So the
*syntax* for such metadata is an easy addition, but the *behaviour* (scoring, false-positive handling)
is new logic outside the core matcher, not something Vectorscan provides.

Other future extensions (e.g. `rule`, `chain`, entity-level metadata, additional pattern attributes)
should be added as explicit syntax in this specification and implemented in separate PRs. Existing
Phase-1 syntax should remain backward-compatible.
