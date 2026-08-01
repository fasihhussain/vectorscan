/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * grammar_parser.cpp - see grammar_parser.h. Uses only plain string handling (no <regex>/<filesystem>)
 * to match the rest of the core library.
 */

#include "grammar/grammar_parser.h"

#include "hs_compile.h" // HS_FLAG_*

#include <cstdlib>
#include <fstream>

namespace ue2 {
namespace grammar {

static std::string baseName(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// import "<spec>"  ->  returns the spec (between the quotes), or false if not an import line.
static bool parseImport(const std::string &line, std::string &spec) {
    if (line.compare(0, 6, "import") != 0) {
        return false;
    }
    size_t q1 = line.find('"');
    if (q1 == std::string::npos) {
        return false;
    }
    size_t q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return false;
    }
    spec = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

// entity <name>:  ->  returns <name>, or false if not an entity header.
static bool parseEntity(const std::string &line, std::string &name) {
    if (line.compare(0, 7, "entity ") != 0 || line.back() != ':') {
        return false;
    }
    name = trim(line.substr(7, line.size() - 8)); // between "entity " and trailing ':'
    return !name.empty();
}

// <id>:/<regex>/<flags>  ->  fills id/regex/flags, or false if the line isn't a pattern.
static bool parsePattern(const std::string &line, unsigned &id, std::string &regex,
                         std::string &flags) {
    size_t i = 0;
    if (i >= line.size() || !isdigit((unsigned char)line[i])) {
        return false;
    }
    size_t idStart = i;
    while (i < line.size() && isdigit((unsigned char)line[i])) {
        i++;
    }
    std::string idStr = line.substr(idStart, i - idStart);
    if (i >= line.size() || line[i] != ':') {
        return false;
    }
    i++;
    if (i >= line.size() || line[i] != '/') {
        return false;
    }
    size_t open = i;                       // first '/'
    size_t close = line.find_last_of('/'); // last '/'
    if (close <= open) {
        return false;
    }
    id = (unsigned)std::strtoul(idStr.c_str(), nullptr, 10);
    regex = line.substr(open + 1, close - open - 1);
    flags = line.substr(close + 1);
    return true;
}

bool parseHsgFile(const std::string &path, HsgFile &out, std::string &err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    const std::string base = baseName(path);
    std::string curEntity; // patterns before any `entity <name>:` belong to "" (default)
    std::string raw;
    while (std::getline(f, raw)) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::string spec, name, regex, flags;
        unsigned id;
        if (parseImport(line, spec)) {
            out.imports.push_back(spec);
        } else if (parseEntity(line, name)) {
            curEntity = name;
        } else if (parsePattern(line, id, regex, flags)) {
            out.patterns.push_back({id, regex, flags, base, curEntity});
        }
    }
    return true;
}

unsigned mapFlags(const std::string &s) {
    unsigned f = 0;
    for (char c : s) {
        switch (c) {
        case 'i': f |= HS_FLAG_CASELESS;     break;
        case 's': f |= HS_FLAG_DOTALL;       break;
        case 'm': f |= HS_FLAG_MULTILINE;    break;
        case 'H': f |= HS_FLAG_SINGLEMATCH;  break;
        case 'V': f |= HS_FLAG_ALLOWEMPTY;   break;
        case 'W': f |= HS_FLAG_UCP;          break;
        case '8': f |= HS_FLAG_UTF8;         break;
        case 'P': f |= HS_FLAG_PREFILTER;    break;
        case 'L': f |= HS_FLAG_SOM_LEFTMOST; break;
        case 'C': f |= HS_FLAG_COMBINATION;  break;
        case 'Q': f |= HS_FLAG_QUIET;        break;
        default: break; // unknown letters ignored
        }
    }
    return f;
}

} // namespace grammar
} // namespace ue2
