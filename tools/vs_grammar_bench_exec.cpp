/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * vs_grammar_bench_exec.cpp — minimal Vectorscan grammar benchmark executable.
 *
 * Compiles a grammar (.hsg or .spec) through the PUBLIC hs_compile_multi() + HS_FLAG_GRAMMAR_REF
 * path (the same path callers use), scans a corpus in block mode, measures compile + scan time,
 * counts matches, and emits machine-readable JSON to stdout. It contains NO optimization logic and
 * NO Claude logic — it is the measurement primitive driven by tools/vs_grammar_bench.py.
 *
 * JSON (success):
 *   {"vectorscan":{"compile_ms":..,"scan_ms":..,"matches":N,"corpus_bytes":N,
 *                  "throughput_mb_s":..,"detections":[{"id":..,"from":..,"to":..},...],
 *                  "detections_truncated":false}}
 * JSON (failure): {"error":"compile_failed","rc":<int>,"message":"..."}  (exit 1)
 * (matched text is not emitted; the Python layer recovers it from the corpus via from/to.)
 */
#include "hs.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
struct Det { unsigned id; unsigned long long from, to; };
struct Ctx { std::vector<Det> dets; size_t total = 0; size_t cap = 0; };

int onMatch(unsigned id, unsigned long long from, unsigned long long to, unsigned, void *c) {
    Ctx *x = static_cast<Ctx *>(c);
    x->total++;
    if (x->dets.size() < x->cap) x->dets.push_back({id, from, to});
    return 0;
}

std::string jsonEscape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}
} // namespace

int main(int argc, char **argv) {
    std::string grammar, corpus;
    size_t maxDet = 200000; // cap detections array so huge corpora don't OOM the JSON; count stays full
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--grammar" && i + 1 < argc) grammar = argv[++i];
        else if (a == "--corpus" && i + 1 < argc) corpus = argv[++i];
        else if (a == "--max-detections" && i + 1 < argc) maxDet = std::strtoul(argv[++i], nullptr, 10);
        else { std::fprintf(stderr, "unknown/incomplete arg: %s\n", a.c_str()); return 2; }
    }
    if (grammar.empty() || corpus.empty()) {
        std::fprintf(stderr, "usage: vs_grammar_bench --grammar <path> --corpus <path> [--max-detections N]\n");
        return 2;
    }

    std::ifstream cf(corpus, std::ios::binary);
    if (!cf) { std::fprintf(stderr, "cannot open corpus: %s\n", corpus.c_str()); return 2; }
    std::string data((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());

    const char *expr[] = { grammar.c_str() };
    unsigned flags[] = { HS_FLAG_GRAMMAR_REF };
    unsigned ids[] = { 0 };
    hs_database_t *db = nullptr; hs_compile_error_t *err = nullptr;

    auto t0 = std::chrono::steady_clock::now();
    hs_error_t rc = hs_compile_multi(expr, flags, ids, 1, HS_MODE_BLOCK, nullptr, &db, &err);
    auto t1 = std::chrono::steady_clock::now();
    if (rc != HS_SUCCESS) {
        std::string m = (err && err->message) ? err->message : "compile error";
        if (err) hs_free_compile_error(err);
        std::printf("{\"error\":\"compile_failed\",\"rc\":%d,\"message\":\"%s\"}\n", (int)rc, jsonEscape(m).c_str());
        return 1;
    }
    double compile_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    hs_scratch_t *scr = nullptr;
    if (hs_alloc_scratch(db, &scr) != HS_SUCCESS) {
        std::printf("{\"error\":\"scratch_alloc_failed\"}\n"); hs_free_database(db); return 1;
    }
    Ctx ctx; ctx.cap = maxDet;
    auto s0 = std::chrono::steady_clock::now();
    hs_error_t sc = hs_scan(db, data.data(), (unsigned)data.size(), 0, scr, onMatch, &ctx);
    auto s1 = std::chrono::steady_clock::now();
    if (sc != HS_SUCCESS && sc != HS_SCAN_TERMINATED) {
        std::printf("{\"error\":\"scan_failed\",\"rc\":%d}\n", (int)sc);
        hs_free_scratch(scr); hs_free_database(db); return 1;
    }
    double scan_ms = std::chrono::duration<double, std::milli>(s1 - s0).count();
    double mb = (double)data.size() / (1024.0 * 1024.0);
    double thr = scan_ms > 0.0 ? mb / (scan_ms / 1000.0) : 0.0;

    std::printf("{\"vectorscan\":{");
    std::printf("\"compile_ms\":%.4f,\"scan_ms\":%.4f,\"matches\":%zu,\"corpus_bytes\":%zu,\"throughput_mb_s\":%.4f,",
                compile_ms, scan_ms, ctx.total, data.size(), thr);
    std::printf("\"detections\":[");
    for (size_t i = 0; i < ctx.dets.size(); i++) {
        std::printf("%s{\"id\":%u,\"from\":%llu,\"to\":%llu}", i ? "," : "",
                    ctx.dets[i].id, ctx.dets[i].from, ctx.dets[i].to);
    }
    std::printf("],\"detections_truncated\":%s}}\n", (ctx.total > ctx.dets.size()) ? "true" : "false");

    hs_free_scratch(scr); hs_free_database(db);
    return 0;
}
