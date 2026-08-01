/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * import_resolver.cpp - see import_resolver.h. Plain string / POSIX realpath path handling only.
 */

#include "grammar/import_resolver.h"

#include <cstdlib> // realpath, free
#include <set>

namespace ue2 {
namespace grammar {

static std::string dirName(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static std::string baseName(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Canonical absolute key for cycle detection (falls back to the raw path if the file is absent).
static std::string canonical(const std::string &path) {
    char *r = realpath(path.c_str(), nullptr);
    if (!r) {
        return path;
    }
    std::string out(r);
    free(r);
    return out;
}

// An import target is "path" (whole file) or "path:entity" (only that entity). Split at ".hsg:".
static void splitSpec(const std::string &spec, std::string &path, std::string &entity) {
    const std::string mark = ".hsg:";
    size_t p = spec.find(mark);
    if (p != std::string::npos) {
        path = spec.substr(0, p + 4);
        entity = spec.substr(p + 5);
    } else {
        path = spec;
        entity.clear();
    }
}

// onStack = files on the current whole-file resolution path (cycle detection).
// done     = (file|entity) keys already included (de-dup). Imports resolved before own patterns.
static bool rec(const std::string &path, const std::string &entity,
                std::set<std::string> &onStack, std::set<std::string> &done,
                std::vector<HsgPattern> &out, std::vector<std::string> &order,
                std::string &err) {
    const std::string canon = canonical(path);
    const std::string fname = baseName(path);
    const std::string tag = fname + (entity.empty() ? "" : (":" + entity));
    const std::string key = canon + "|" + entity;

    if (entity.empty() && onStack.count(canon)) {
        err = "CYCLE detected at " + fname;
        return false;
    }
    if (done.count(key)) {
        order.push_back(tag + " [already resolved -> de-duped]");
        return true;
    }

    HsgFile gf;
    if (!parseHsgFile(path, gf, err)) {
        return false;
    }
    done.insert(key);
    order.push_back(tag);

    if (entity.empty()) {
        // whole file: resolve imports (recursively) then add every pattern
        onStack.insert(canon);
        const std::string dir = dirName(path);
        for (const auto &imp : gf.imports) {
            std::string ipath, ient;
            splitSpec(imp, ipath, ient);
            const std::string child = dir.empty() ? ipath : dir + "/" + ipath;
            if (!rec(child, ient, onStack, done, out, order, err)) {
                return false;
            }
        }
        for (const auto &p : gf.patterns) {
            out.push_back(p);
        }
        onStack.erase(canon);
    } else {
        // entity-addressed: include ONLY that named entity's own patterns (selective import)
        unsigned n = 0;
        for (const auto &p : gf.patterns) {
            if (p.entity == entity) {
                out.push_back(p);
                n++;
            }
        }
        if (n == 0) {
            err = "entity '" + entity + "' not found in " + fname;
            return false;
        }
    }
    return true;
}

bool resolveGrammar(const std::string &rootPath, std::vector<HsgPattern> &out,
                    std::vector<std::string> &order, std::string &err) {
    std::set<std::string> onStack, done;
    return rec(rootPath, /*entity=*/"", onStack, done, out, order, err);
}

} // namespace grammar
} // namespace ue2
