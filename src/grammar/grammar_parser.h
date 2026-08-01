/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * grammar_parser.h - parse a single native .hsg grammar file.
 *
 * A .hsg file is Vectorscan's own grammar format (no XML, no external format):
 *   import "other.hsg"            - pull in a whole grammar file
 *   import "other.hsg:entity"     - pull in only one named entity from that file
 *   entity <name>:                - group the following pattern lines under a named entity
 *   <id>:/<regex>/<flags>         - a pattern; <flags> are Vectorscan flag letters (i,s,m,H,V,W,8,P,L,C,Q)
 *
 * This header only parses ONE file; recursion/cycle-detection lives in import_resolver.
 */

#ifndef GRAMMAR_GRAMMAR_PARSER_H
#define GRAMMAR_GRAMMAR_PARSER_H

#include <string>
#include <vector>

namespace ue2 {
namespace grammar {

// One pattern line. `entity` is the `entity <name>:` section it appeared in ("" = default section).
struct HsgPattern {
    unsigned id;
    std::string regex;
    std::string flagstr;
    std::string source;
    std::string entity;
};

// One parsed file: its import directives (each "path" or "path:entity") and its pattern lines.
struct HsgFile {
    std::vector<std::string> imports;
    std::vector<HsgPattern> patterns;
};

// Parse one .hsg file. Returns false and sets err on I/O failure.
bool parseHsgFile(const std::string &path, HsgFile &out, std::string &err);

// Map a .hsg flag field (Vectorscan flag letters) to real HS_FLAG_* bits. Matches
// util/ExpressionParser.rl / readExpression() exactly (i=CASELESS, s=DOTALL, m=MULTILINE,
// H=SINGLEMATCH, V=ALLOWEMPTY, W=UCP, 8=UTF8, P=PREFILTER, L=SOM_LEFTMOST, C=COMBINATION, Q=QUIET).
unsigned mapFlags(const std::string &flagstr);

} // namespace grammar
} // namespace ue2

#endif // GRAMMAR_GRAMMAR_PARSER_H
