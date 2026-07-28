/*
 * grammar_compile.cpp — public API: hs_compile_from_grammar_files().
 *
 * Ties the grammar-import layer to the existing compiler: read grammar files
 * (+ their <include>s) -> resolve the requested reference -> regex-escape the resolved
 * literal entries -> hs_compile_multi() (the PRIMARY production path; hs_compile_lit_multi
 * is a later optimization-only route for pure-literal dictionaries, not used here).
 *
 * Engine internals (nfa/, rose/, compiler/) are untouched — this is preprocessing that
 * hands finished patterns to the existing multi-pattern compiler.
 */
#include "hs_compile.h"
#include "grammar_import.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using ue2::grammar::Registry;
using ue2::grammar::ResolveResult;

namespace {

// Directory portion of a path ("" if none), for resolving includes relative to the file.
std::string dirOf(const std::string &p) {
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? std::string() : p.substr(0, pos);
}

// Regex-escape a literal so hs_compile_multi (a REGEX compiler) matches it verbatim.
std::string escapeLiteral(const std::string &s) {
    static const std::string META = R"(.^$*+?()[]{}|\)";
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (META.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

// INTEGRATION NOTE (verify against the repo before merge):
//   hs_free_compile_error() frees the returned error with Hyperscan's configured
//   allocator. Allocate the error the SAME way the existing compile path does (see how
//   compiler.cpp / util build hs_compile_error_t, e.g. the internal error helper) so that
//   hs_free_compile_error() is safe. The malloc below is a placeholder for standalone
//   review; wire it to the internal allocator during integration.
hs_compile_error_t *makeError(const std::string &msg) {
    auto *e = static_cast<hs_compile_error_t *>(malloc(sizeof(hs_compile_error_t)));
    if (!e) return nullptr;
    e->message = static_cast<char *>(malloc(msg.size() + 1));
    if (e->message) memcpy(e->message, msg.c_str(), msg.size() + 1);
    e->expression = -1;
    return e;
}

}  // namespace

extern "C" hs_error_t HS_CDECL hs_compile_from_grammar_files(
    const char *const *paths, unsigned int num_paths, const char *reference,
    unsigned int flags, unsigned int mode, const hs_platform_info_t *platform,
    hs_database_t **db, hs_compile_error_t **error) {

    if (!paths || num_paths == 0 || !reference || !db) {
        if (error) *error = makeError("null/empty argument to hs_compile_from_grammar_files");
        return HS_COMPILER_ERROR;
    }

    // 1. Build the registry from every supplied grammar file + its <include>s.
    //    Grammar names declared directly in the supplied files form the "local" scope.
    Registry reg;
    std::set<std::string> localGrammars;
    for (unsigned i = 0; i < num_paths; i++) {
        const std::string path = paths[i];
        const std::string xml = ue2::grammar::readStripped(path);
        if (xml.empty()) {
            if (error) *error = makeError("cannot read grammar file: " + path);
            return HS_COMPILER_ERROR;
        }
        std::string perr;
        if (!ue2::grammar::parseGrammarXml(xml, reg, perr)) {
            if (error) *error = makeError(perr + " (" + path + ")");
            return HS_COMPILER_ERROR;
        }
        for (const auto &g : ue2::grammar::grammarNames(xml)) localGrammars.insert(g);

        const std::string base = dirOf(path);
        for (const auto &inc : ue2::grammar::parseIncludePaths(xml)) {
            // readable source: <stem>.decompiled.xml preferred, else <stem>.xml
            std::string stem = inc;
            auto slash = stem.find_last_of('/'); if (slash != std::string::npos) stem = stem.substr(slash + 1);
            auto dot = stem.find_last_of('.');   if (dot != std::string::npos) stem = stem.substr(0, dot);
            for (const std::string &cand : {stem + ".decompiled.xml", stem + ".xml"}) {
                const std::string full = base.empty() ? cand : base + "/" + cand;
                std::ifstream probe(full);
                if (probe.good()) {
                    std::string ierr;
                    ue2::grammar::parseGrammarXml(ue2::grammar::readStripped(full), reg, ierr);
                    break;
                }
            }
        }
    }

    // 2. Resolve the reference (composite/nested entities rejected here).
    ResolveResult res = ue2::grammar::resolveReference(reference, reg, localGrammars);
    if (!res.ok) {
        if (error) *error = makeError(res.status);
        return HS_COMPILER_ERROR;
    }
    if (res.entries.empty()) {
        if (error) *error = makeError("reference resolved to zero entries: " + std::string(reference));
        return HS_COMPILER_ERROR;
    }

    // 3. Escape resolved literals and hand off to the existing multi-pattern compiler.
    std::vector<std::string> escaped;
    escaped.reserve(res.entries.size());
    for (const auto &e : res.entries) escaped.push_back(escapeLiteral(e));

    std::vector<const char *> exprs;
    std::vector<unsigned int> flagv, idv;
    exprs.reserve(escaped.size());
    flagv.reserve(escaped.size());
    idv.reserve(escaped.size());
    for (size_t i = 0; i < escaped.size(); i++) {
        exprs.push_back(escaped[i].c_str());
        flagv.push_back(flags);
        idv.push_back(static_cast<unsigned int>(i));
    }

    // hs_compile_multi fills *error itself on a compile failure — pass it straight through.
    return hs_compile_multi(exprs.data(), flagv.data(), idv.data(),
                            static_cast<unsigned int>(exprs.size()), mode, platform, db, error);
}
