/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * grammar_import.cpp - see grammar_import.h.
 */

#include "grammar/grammar_import.h"

#include "grammar/import_resolver.h"
#include "grammar/grammar_parser.h"
#include "hs_compile.h" // HS_FLAG_GRAMMAR_REF

namespace ue2 {
namespace grammar {

static bool endsWithHsg(const std::string &s) {
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".hsg") == 0;
}

bool expandGrammarRefs(const char *const *expressions, const unsigned *flags,
                       const unsigned *ids, unsigned elements,
                       std::vector<std::string> &outExpr, std::vector<unsigned> &outFlags,
                       std::vector<unsigned> &outIds, std::string &err) {
    for (unsigned i = 0; i < elements; i++) {
        const std::string e = expressions[i];
        const unsigned fl = flags ? flags[i] : 0;
        const unsigned id = ids ? ids[i] : i;

        if (fl & HS_FLAG_GRAMMAR_REF) {
            // The flag explicitly says "this is a grammar file" — it MUST be a .hsg, otherwise the
            // caller has misused the flag. Do not silently fall back to compiling it as a regex.
            if (!endsWithHsg(e)) {
                err = "HS_FLAG_GRAMMAR_REF is set but the expression is not a .hsg file: " + e;
                return false;
            }
            std::vector<HsgPattern> pats;
            std::vector<std::string> order;
            if (!resolveGrammar(e, pats, order, err)) {
                return false; // clean error (cycle / bad path / missing entity / syntax)
            }
            for (const auto &p : pats) {
                outExpr.push_back(p.regex);
                outFlags.push_back(mapFlags(p.flagstr));
                outIds.push_back(p.id);
            }
        } else {
            outExpr.push_back(e);
            outFlags.push_back(fl); // no grammar marker to strip
            outIds.push_back(id);
        }
    }
    return true;
}

} // namespace grammar
} // namespace ue2
