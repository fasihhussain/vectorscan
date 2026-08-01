/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * import_resolver.h - resolve a .hsg file's `import` directives into a flat pattern list.
 *
 * Handles both whole-file imports (import "f.hsg") and entity-addressed imports
 * (import "f.hsg:entity"), resolved recursively, with cycle detection and de-duplication.
 */

#ifndef GRAMMAR_IMPORT_RESOLVER_H
#define GRAMMAR_IMPORT_RESOLVER_H

#include "grammar/grammar_parser.h"

#include <string>
#include <vector>

namespace ue2 {
namespace grammar {

// Resolve rootPath and all its (recursive) imports into `out`. `order` records the resolution
// order (for diagnostics/tests). Returns false and sets err on cycle / bad path / missing entity.
bool resolveGrammar(const std::string &rootPath, std::vector<HsgPattern> &out,
                    std::vector<std::string> &order, std::string &err);

} // namespace grammar
} // namespace ue2

#endif // GRAMMAR_IMPORT_RESOLVER_H
