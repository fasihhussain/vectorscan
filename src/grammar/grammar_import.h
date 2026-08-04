/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * grammar_import.h - bridge between hs_compile_multi() and the .hsg grammar resolver.
 */

#ifndef GRAMMAR_GRAMMAR_IMPORT_H
#define GRAMMAR_GRAMMAR_IMPORT_H

#include <string>
#include <vector>

namespace ue2 {
namespace grammar {

// Expand a multi-pattern input: any expression whose flag has HS_FLAG_GRAMMAR_REF set and that names
// a .hsg file is resolved (imports + patterns) into plain expressions; every other expression passes
// through unchanged (with the HS_FLAG_GRAMMAR_REF marker bit stripped). The resolved strings are owned
// by outExpr, so hand outExpr[i].c_str() to the compiler. Returns false and sets err on failure.
bool expandGrammarRefs(const char *const *expressions, const unsigned *flags,
                       const unsigned *ids, unsigned elements,
                       std::vector<std::string> &outExpr, std::vector<unsigned> &outFlags,
                       std::vector<unsigned> &outIds, std::string &err);

} // namespace grammar
} // namespace ue2

#endif // GRAMMAR_GRAMMAR_IMPORT_H
