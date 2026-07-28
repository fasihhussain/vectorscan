/*
 * grammar_import.h — internal interface for native Eduction-style grammar import.
 *
 * Resolves <include> directives and (?A^grammar/entity) / (?A:name) references in
 * Eduction grammar XML files to their underlying dictionary/pattern entries. Used by
 * the public API entry point hs_compile_from_grammar_files() (see hs_compile.h /
 * grammar_compile.cpp). This is a grammar-preprocessing layer; it does NOT touch the
 * regex engine internals — resolved patterns are handed to hs_compile_multi().
 *
 * Logic validated standalone before integration (resolve->366 parity with the Python
 * reference resolver; DOCTYPE-safe; composite rejection; hs_compile_multi end-to-end).
 */
#ifndef GRAMMAR_GRAMMAR_IMPORT_H
#define GRAMMAR_GRAMMAR_IMPORT_H

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ue2 {
namespace grammar {

// grammar-name -> entity-name -> entries (dictionary headwords and/or literal <pattern>s)
using Registry = std::map<std::string, std::map<std::string, std::vector<std::string>>>;

// Read a grammar XML file and strip its <!DOCTYPE ...> before returning the text.
// The grammars ship "<!DOCTYPE grammars SYSTEM \"edk.dtd\">"; the XML reader must not
// see it (external-DTD / DOCTYPE handling is parser-dependent), so we remove it first.
std::string readStripped(const std::string &path);

// Parse a (DOCTYPE-stripped) grammar XML string and merge its
// <grammar name>/<entity name>/<entry headword>|<pattern> into `reg` (first source wins).
// Returns false and sets `errMsg` on XML parse failure.
bool parseGrammarXml(const std::string &xml, Registry &reg, std::string &errMsg);

// Extract the paths from a main grammar's <include path="..."/> directives.
std::vector<std::string> parseIncludePaths(const std::string &mainXml);

// Grammar names declared directly in a (stripped) grammar XML — the "local" scope
// used to resolve bare (?A:name) / no-slash references.
std::set<std::string> grammarNames(const std::string &xml);

struct ResolveResult {
    bool ok = false;
    std::string status;                       // "OK" or a human-readable error
    std::string grammar;                       // grammar the entity resolved in
    std::string entity;                        // entity name
    std::vector<std::string> entries;          // resolved dictionary/pattern entries
};

// Resolve ONE reference against the registry.
//   (?A^grammar/entity/path) : leading segment = grammar name (disambiguates entities
//                              that share a name across grammars)
//   (?A:name)  / bare no-slash: resolved in `localGrammars` first, then uniquely anywhere
// A resolved entity whose entries themselves contain references (e.g. "(?A^...)") is a
// COMPOSITE — rejected here (ok=false) because phase-1 compiles pure dictionary/literal
// entities only; nested-reference expansion (combination) is a separate phase.
ResolveResult resolveReference(const std::string &reference, const Registry &reg,
                               const std::set<std::string> &localGrammars);

}  // namespace grammar
}  // namespace ue2

#endif  // GRAMMAR_GRAMMAR_IMPORT_H
