/*
 * reference.cpp — (?A^grammar/entity) / (?A:name) reference resolution.
 *
 * Resolution rules (validated against the Python reference resolver, 56/56 refs on the
 * real grammar set):
 *   - path with '/'  : leading segment is the GRAMMAR name; the rest is the entity name.
 *                       This disambiguates entities that share a name across grammars
 *                       (e.g. "names/single_token" vs "rare_names/single_token").
 *   - bare path       : looked up in the local grammar(s) first, then uniquely anywhere.
 *   - composite entity: entries that themselves contain "(?A^...)"/"(?A:...)" are rejected
 *                        (phase-1 = pure dictionary/literal entities only).
 */
#include "grammar_import.h"

#include <regex>

namespace ue2 {
namespace grammar {

static const std::regex RE_REF(R"(\(\?A([\^:])([^)]+)\))");
static const std::regex RE_NESTED(R"(\(\?A[\^:])");  // an entry that is itself a reference

static std::vector<std::string> split(const std::string &s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

ResolveResult resolveReference(const std::string &reference, const Registry &reg,
                               const std::set<std::string> &localGrammars) {
    ResolveResult r;
    std::smatch m;
    if (!std::regex_search(reference, m, RE_REF)) {
        r.status = "not a valid (?A^...)/(?A:...) reference";
        return r;
    }
    const std::string path = m[2].str();
    const auto segs = split(path, '/');
    const std::vector<std::string> *found = nullptr;

    // 1) <grammar>/<entity> — disambiguate by the leading grammar-name segment
    if (segs.size() > 1) {
        const std::string g = segs[0];
        const std::string ent = path.substr(g.size() + 1);
        auto gi = reg.find(g);
        if (gi != reg.end()) {
            auto ei = gi->second.find(ent);
            if (ei != gi->second.end()) { found = &ei->second; r.grammar = g; r.entity = ent; }
        }
    }
    // 2) bare path in a local grammar
    if (!found) {
        for (const auto &g : localGrammars) {
            auto gi = reg.find(g);
            if (gi == reg.end()) continue;
            auto ei = gi->second.find(path);
            if (ei != gi->second.end()) { found = &ei->second; r.grammar = g + " (local)"; r.entity = path; break; }
        }
    }
    // 3) fallback: whole path as an entity name anywhere (must be unique)
    if (!found) {
        int hits = 0;
        for (const auto &g : reg) {
            auto ei = g.second.find(path);
            if (ei != g.second.end()) { ++hits; found = &ei->second; r.grammar = g.first; r.entity = path; }
        }
        if (hits > 1) { r.status = "AMBIGUOUS — '" + path + "' defined in multiple grammars"; return r; }
    }

    if (!found) { r.status = "NOT FOUND — no grammar/entity for '" + path + "'"; return r; }

    // composite / nested-reference guard (phase-1: pure dictionary entities only)
    for (const auto &e : *found) {
        if (std::regex_search(e, RE_NESTED)) {
            r.status = "COMPOSITE — entity '" + r.entity +
                       "' contains unresolved references; phase-1 native compile supports pure "
                       "dictionary/literal entities only (nested-reference expansion is phase-2 / combination)";
            return r;
        }
    }

    r.ok = true;
    r.status = "OK";
    r.entries = *found;
    return r;
}

}  // namespace grammar
}  // namespace ue2
