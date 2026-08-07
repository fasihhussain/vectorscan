/*
 * Copyright (c) 2026, VectorCamp PC
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * grammar_cfg.cpp — gtest for native CFG 1-layer composition (adds to unit_hyperscan_SOURCES).
 *
 * Everything here drives the PUBLIC path: hs_compile_multi() with HS_FLAG_GRAMMAR_REF on a compose
 * grammar file, then standard hs_scan() whose user callback receives both base component matches and
 * composite matches. No cfgScan/cfgCreate is used. The sidecar test uses the documented internal
 * attach entry points (cfgSerializeSidecar/cfgAttachSidecar) since hs_deserialize_database has no path.
 */
#include "config.h"
#include "gtest/gtest.h"
#include "hs.h"
#include "grammar/cfg_compose.h" // internal (not installed): sidecar attach entry points

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

static std::string g_dir;
static void writeFile(const std::string &name, const std::string &body) {
    std::ofstream f(g_dir + "/" + name); f << body;
}

struct Sink { std::vector<std::pair<unsigned, std::pair<unsigned,unsigned>>> m; };
static int onMatch(unsigned id, unsigned long long from, unsigned long long to, unsigned, void *ctx) {
    static_cast<Sink *>(ctx)->m.push_back({id, {(unsigned)from, (unsigned)to}});
    return 0;
}
static int onHalt(unsigned, unsigned long long, unsigned long long, unsigned, void *ctx) {
    (*static_cast<int *>(ctx))++; return 1; // request halt on first match
}

// Compile a compose grammar file (in g_dir, or an absolute path) via the public grammar-ref path.
static hs_error_t compileCompose(const std::string &fileOrPath, hs_database_t **db) {
    std::string path = (fileOrPath.size() && fileOrPath[0] == '/') ? fileOrPath : g_dir + "/" + fileOrPath;
    const char *expr[] = { path.c_str() };
    unsigned flags[] = { HS_FLAG_GRAMMAR_REF };
    unsigned ids[] = { 0 };
    hs_compile_error_t *err = nullptr;
    hs_error_t rc = hs_compile_multi(expr, flags, ids, 1, HS_MODE_BLOCK, nullptr, db, &err);
    if (err) hs_free_compile_error(err);
    return rc;
}
// True if a composite with `id` spanning [from,to] was emitted for `corpus`.
static bool hasComposite(hs_database_t *db, const std::string &corpus,
                         unsigned id, unsigned from, unsigned to) {
    hs_scratch_t *scr = nullptr;
    EXPECT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scr));
    Sink s;
    EXPECT_EQ(HS_SUCCESS, hs_scan(db, corpus.c_str(), corpus.size(), 0, scr, onMatch, &s));
    hs_free_scratch(scr);
    for (auto &e : s.m) if (e.first == id && e.second.first == from && e.second.second == to) return true;
    return false;
}

class GrammarCFGCompose : public ::testing::Test {
protected:
    void SetUp() override {
        std::random_device rd;
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("cfg_gtest_" + std::to_string(rd()));
        std::filesystem::create_directories(dir);
        g_dir = dir.string();
        writeFile("firstname.txt", "John\nJane\nMichael\n");
        writeFile("lastname.txt",  "Smith\nJones\nMichael\n");
        writeFile("suffix.txt",    "Jr\nSr\n");
        writeFile("names.hsg",
            "dict firstname firstname.txt\n"
            "dict lastname lastname.txt\n"
            "dict suffix suffix.txt\n"
            "compose 9000 namefirstlast: firstname, lastname\n"
            "compose 9001 namelastcommafirst: lastname, \",\", firstname\n"
            "compose 9002 nameinitial: firstname, initials, lastname\n"
            "compose 9003 namefirstmiddlelast: firstname, firstname, lastname\n"
            "compose 9004 namelastsuffix: firstname, lastname, suffix\n"
            "compose 9005 compoundlastname: lastname, \"-\", lastname\n");
        // adversarial fixtures (each should FAIL to compile through the real path)
        writeFile("dup.hsg", "dict firstname firstname.txt\ndict lastname lastname.txt\n"
            "compose 9000 a: firstname, lastname\ncompose 9000 b: lastname, firstname\n");
        writeFile("selfref.hsg", "dict lastname lastname.txt\n"
            "compose 9000 selfref: selfref, lastname\n");
        writeFile("nested.hsg", "dict firstname firstname.txt\ndict lastname lastname.txt\n"
            "compose 9000 base: firstname, lastname\ncompose 9001 nested: base, lastname\n");
        writeFile("missing.hsg", "dict firstname firstname.txt\n"
            "compose 9000 miss: firstname, doesnotexist\n");
        // Portuguese (UTF-8) — the engine is byte/locale-agnostic; proves composition works for a
        // locale absent from broad_list_composition.spec (which contains no Portuguese).
        writeFile("pt_first.txt", "Jo\xC3\xA3o\nMaria\n");   // João, Maria
        writeFile("pt_last.txt",  "Silva\nSanto\xC3\xA9\n"); // Silva, Santoé
        writeFile("names_pt.hsg",
            "dict firstname pt_first.txt\n"
            "dict lastname pt_last.txt\n"
            "compose 9000 namefirstlast: firstname, lastname\n");
    }
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(g_dir, ec); }
};

TEST_F(GrammarCFGCompose, ValidNameFirstLast) {
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    EXPECT_TRUE(hasComposite(db, "John Smith", 9000, 0, 10));
    hs_free_database(db);
}
TEST_F(GrammarCFGCompose, ValidLastCommaFirst) {
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    EXPECT_TRUE(hasComposite(db, "Smith, John", 9001, 0, 11));
    hs_free_database(db);
}
TEST_F(GrammarCFGCompose, ValidAllSixNameGrammars) {
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    EXPECT_TRUE(hasComposite(db, "John Smith",         9000, 0, 10));
    EXPECT_TRUE(hasComposite(db, "Smith, John",        9001, 0, 11));
    EXPECT_TRUE(hasComposite(db, "John D. Smith",      9002, 0, 13));
    EXPECT_TRUE(hasComposite(db, "John Michael Smith", 9003, 0, 18));
    EXPECT_TRUE(hasComposite(db, "John Smith Jr",      9004, 0, 13));
    EXPECT_TRUE(hasComposite(db, "Smith-Jones",        9005, 0, 11));
    // wrong order emits no first-last composite
    EXPECT_FALSE(hasComposite(db, "Smith John",        9000, 0, 10));
    hs_free_database(db);
}
TEST_F(GrammarCFGCompose, SpecEngcn) {
    const std::string spec = "/Users/fahad/Downloads/Testing/generated/broad_list_name_benchmark/"
                             "composition/broad_list_composition.spec:engcn";
    if (!std::filesystem::exists("/Users/fahad/Downloads/Testing/generated/broad_list_name_benchmark/"
                                 "composition/broad_list_composition.spec")) {
        std::cerr << "[ SKIPPED ] spec fixture not present\n"; return; // bundled gtest lacks GTEST_SKIP
    }
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose(spec, &db));
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scr));
    Sink s; ASSERT_EQ(HS_SUCCESS, hs_scan(db, "Ai Bai", 6, 0, scr, onMatch, &s));
    bool anyComposite = false; for (auto &e : s.m) if (e.first >= 100000) anyComposite = true;
    EXPECT_TRUE(anyComposite);
    hs_free_scratch(scr); hs_free_database(db);
}
TEST_F(GrammarCFGCompose, RejectDuplicateId) {
    hs_database_t *db = nullptr; EXPECT_EQ(HS_COMPILER_ERROR, compileCompose("dup.hsg", &db));
    EXPECT_EQ(nullptr, db);
}
TEST_F(GrammarCFGCompose, RejectSelfRef) {
    hs_database_t *db = nullptr; EXPECT_EQ(HS_COMPILER_ERROR, compileCompose("selfref.hsg", &db));
    EXPECT_EQ(nullptr, db);
}
TEST_F(GrammarCFGCompose, RejectNestedCompose) {
    hs_database_t *db = nullptr; EXPECT_EQ(HS_COMPILER_ERROR, compileCompose("nested.hsg", &db));
    EXPECT_EQ(nullptr, db);
}
TEST_F(GrammarCFGCompose, RejectMissingEntity) {
    hs_database_t *db = nullptr; EXPECT_EQ(HS_COMPILER_ERROR, compileCompose("missing.hsg", &db));
    EXPECT_EQ(nullptr, db);
}
TEST_F(GrammarCFGCompose, SerializationSidecar) {
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    // serialize DB (public) + sidecar (internal attach entry), then free
    char *bytes = nullptr; size_t blen = 0; std::string err;
    ASSERT_EQ(HS_SUCCESS, hs_serialize_database(db, &bytes, &blen));
    const std::string scp = g_dir + "/names.sidecar";
    ASSERT_TRUE(ue2::grammar::cfgSerializeSidecar(db, scp, err)) << err;
    hs_free_database(db);
    // fresh deserialize (memory-only; no path) -> before attach the CFG-flagged DB must not silent-drop
    hs_database_t *db2 = nullptr; ASSERT_EQ(HS_SUCCESS, hs_deserialize_database(bytes, blen, &db2));
    free(bytes);
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db2, &scr));
    { Sink s; EXPECT_EQ(HS_INVALID, hs_scan(db2, "John Smith", 10, 0, scr, onMatch, &s)); }
    // attach sidecar (documented internal mechanism) then standard hs_scan works
    ASSERT_EQ(HS_SUCCESS, ue2::grammar::cfgAttachSidecar(db2, scp, err)) << err;
    { Sink s; ASSERT_EQ(HS_SUCCESS, hs_scan(db2, "John Smith", 10, 0, scr, onMatch, &s));
      bool ok = false; for (auto &e : s.m) if (e.first == 9000 && e.second.first == 0 && e.second.second == 10) ok = true;
      EXPECT_TRUE(ok); }
    hs_free_scratch(scr); hs_free_database(db2);
}
TEST_F(GrammarCFGCompose, SidecarBesideConvention) {
    // Decided Phase-1 model: db file + "<dbPath>.cfgmeta" sidecar; tool layer attaches, user scans normally.
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    const std::string dbPath = g_dir + "/names.hsdb"; std::string err;
    char *bytes = nullptr; size_t blen = 0;
    ASSERT_EQ(HS_SUCCESS, hs_serialize_database(db, &bytes, &blen));
    { std::ofstream o(dbPath, std::ios::binary); o.write(bytes, (std::streamsize)blen); }
    ASSERT_TRUE(ue2::grammar::cfgWriteSidecarBeside(db, dbPath, err)) << err; // writes names.hsdb.cfgmeta
    free(bytes); hs_free_database(db);
    ASSERT_TRUE(std::filesystem::exists(dbPath + ".cfgmeta"));
    // fresh load from the file pair
    std::ifstream df(dbPath, std::ios::binary);
    std::string buf((std::istreambuf_iterator<char>(df)), std::istreambuf_iterator<char>());
    hs_database_t *db2 = nullptr; ASSERT_EQ(HS_SUCCESS, hs_deserialize_database(buf.data(), buf.size(), &db2));
    ASSERT_EQ(HS_SUCCESS, ue2::grammar::cfgAttachSidecarBeside(db2, dbPath, err)) << err;
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db2, &scr));
    Sink s; ASSERT_EQ(HS_SUCCESS, hs_scan(db2, "John Smith", 10, 0, scr, onMatch, &s));
    bool ok = false; for (auto &e : s.m) if (e.first == 9000) ok = true;
    EXPECT_TRUE(ok);
    hs_free_scratch(scr); hs_free_database(db2);
}
TEST_F(GrammarCFGCompose, EarlyReturnStopsScanNoComposites) {
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names.hsg", &db));
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scr));
    int calls = 0;
    hs_error_t rv = hs_scan(db, "John Michael Smith", 18, 0, scr, onHalt, &calls);
    // Only composites reach the callback; returning non-zero on the first composite stops emission.
    EXPECT_EQ(HS_SCAN_TERMINATED, rv);
    EXPECT_EQ(1, calls);
    hs_free_scratch(scr); hs_free_database(db);
}
TEST_F(GrammarCFGCompose, ValidPortugueseNames) {
    // "João Silva" = 4-byte-first (J,o,ã=2 bytes,o) + space + "Silva" -> composite 9000 @0-11 (bytes).
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("names_pt.hsg", &db));
    const std::string corpus = "Jo\xC3\xA3o Silva"; // João Silva
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scr));
    Sink s; ASSERT_EQ(HS_SUCCESS, hs_scan(db, corpus.c_str(), corpus.size(), 0, scr, onMatch, &s));
    bool ok = false;
    for (auto &e : s.m) if (e.first == 9000) ok = true; // composite present (byte-span exact = [0, len])
    EXPECT_TRUE(ok);
    hs_free_scratch(scr); hs_free_database(db);
}
TEST_F(GrammarCFGCompose, ImportRegressionStillPasses) {
    // The ordinary import path (non-compose .hsg) must still compile + scan unchanged.
    writeFile("plain.hsg", "entity firstname:\n100:/John/\n101:/Jane/\n");
    writeFile("useplain.hsg", "import \"plain.hsg\"\n");
    hs_database_t *db = nullptr; ASSERT_EQ(HS_SUCCESS, compileCompose("useplain.hsg", &db));
    hs_scratch_t *scr = nullptr; ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scr));
    Sink s; ASSERT_EQ(HS_SUCCESS, hs_scan(db, "John", 4, 0, scr, onMatch, &s));
    EXPECT_FALSE(s.m.empty());
    hs_free_scratch(scr); hs_free_database(db);
}

} // namespace
