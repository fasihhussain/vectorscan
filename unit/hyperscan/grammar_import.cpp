/*
 * grammar_import.cpp — gtest for native .hsg grammar import (adds to unit_hyperscan_SOURCES).
 *
 * Placement:  unit/hyperscan/grammar_import.cpp
 * Register:   add `hyperscan/grammar_import.cpp` to set(unit_hyperscan_SOURCES ...) in unit/CMakeLists.txt
 * Run:        ./build/bin/unit-hyperscan --gtest_filter='GrammarImport.*'
 *
 * These tests exercise the PUBLIC compile path: hs_compile_multi() with HS_FLAG_GRAMMAR_REF set on an
 * expression that names a .hsg file. The library resolves imports (whole-file OR entity-addressed),
 * then compiles via the normal multi-pattern internals. No Eduction / XML / Boost involved.
 */
#include "config.h"
#include "gtest/gtest.h"
#include "hs.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>

namespace {

// ---- helpers -------------------------------------------------------------------------------------
static std::string g_dir;   // per-run scratch dir holding the fixture .hsg files

static void writeFile(const std::string &name, const std::string &body) {
    std::ofstream f(g_dir + "/" + name);
    f << body;
}

struct MatchSink { std::set<unsigned> ends; };
static int onMatch(unsigned, unsigned long long, unsigned long long to, unsigned, void *ctx) {
    static_cast<MatchSink *>(ctx)->ends.insert(static_cast<unsigned>(to));
    return 0;
}

// Compile a root .hsg via the import-extended hs_compile_multi(). Returns the hs_error_t; on success
// *db is set. Caller frees db. On failure *db stays null and the compile error is freed here.
static hs_error_t compileHsg(const std::string &rootFile, hs_database_t **db) {
    const std::string path = g_dir + "/" + rootFile;
    const char *expr[] = { path.c_str() };
    unsigned flags[]   = { HS_FLAG_GRAMMAR_REF };
    unsigned ids[]     = { 0 };
    hs_compile_error_t *err = nullptr;
    hs_error_t rc = hs_compile_multi(expr, flags, ids, 1, HS_MODE_BLOCK, nullptr, db, &err);
    if (err) hs_free_compile_error(err);
    return rc;
}

// True if some match in `corpus` ends exactly where `word` ends (i.e. `word` was matched).
static bool scanHas(hs_database_t *db, const std::string &corpus, const std::string &word) {
    hs_scratch_t *scratch = nullptr;
    EXPECT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));
    MatchSink sink;
    EXPECT_EQ(HS_SUCCESS, hs_scan(db, corpus.c_str(), corpus.size(), 0, scratch, onMatch, &sink));
    hs_free_scratch(scratch);
    size_t pos = corpus.find(word);
    return pos != std::string::npos && sink.ends.count(static_cast<unsigned>(pos + word.size()));
}

class GrammarImport : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/hsg_gtest_XXXXXX";
        ASSERT_NE(nullptr, mkdtemp(tmpl));
        g_dir = tmpl;
        // a file with two named entities
        writeFile("names.hsg",
                  "entity firstname:\n100:/John/\n101:/Jane/\n"
                  "entity lastname:\n200:/Smith/\n201:/Jones/\n");
        // roots
        writeFile("use_first.hsg", "import \"names.hsg:firstname\"\n300:/Zeta/\n");
        writeFile("use_whole.hsg", "import \"names.hsg\"\n");
        writeFile("use_bad.hsg",   "import \"names.hsg:nope\"\n");
        writeFile("use_missing.hsg","import \"does_not_exist.hsg\"\n");
        // cycle A2 -> B2 -> A2
        writeFile("A2.hsg", "import \"B2.hsg\"\n400:/aaa/\n");
        writeFile("B2.hsg", "import \"A2.hsg\"\n410:/bbb/\n");
        // 4-level chain L1 -> L2 -> L3 -> L4
        writeFile("L1.hsg", "import \"L2.hsg\"\n510:/level1/\n");
        writeFile("L2.hsg", "import \"L3.hsg\"\n520:/level2/\n");
        writeFile("L3.hsg", "import \"L4.hsg\"\n530:/level3/\n");
        writeFile("L4.hsg", "540:/level4/\n");
        // SOM letter-flag: /L applied (id 700) vs none (id 701)
        writeFile("som.hsg", "700:/Zeta/L\n701:/Zeta/\n");
    }
};

// ---- tests ---------------------------------------------------------------------------------------

// Entity-addressed import pulls ONLY the named entity: firstname present, lastname absent.
TEST_F(GrammarImport, EntityAddressed) {
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, compileHsg("use_first.hsg", &db));
    EXPECT_TRUE(scanHas(db, "a John here", "John"));    // firstname entity -> in
    EXPECT_TRUE(scanHas(db, "a Zeta here", "Zeta"));    // local pattern     -> in
    EXPECT_FALSE(scanHas(db, "a Smith here", "Smith")); // lastname entity   -> excluded
    hs_free_database(db);
}

// Whole-file import (no entity spec) pulls every entity.
TEST_F(GrammarImport, WholeFileImport) {
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, compileHsg("use_whole.hsg", &db));
    EXPECT_TRUE(scanHas(db, "a John here", "John"));
    EXPECT_TRUE(scanHas(db, "a Smith here", "Smith"));  // now included
    hs_free_database(db);
}

// A reference to a non-existent entity is a clean compile error (not a crash).
TEST_F(GrammarImport, BadEntityIsCleanError) {
    hs_database_t *db = nullptr;
    EXPECT_EQ(HS_COMPILER_ERROR, compileHsg("use_bad.hsg", &db));
    EXPECT_EQ(nullptr, db);
}

// A missing import path is a clean compile error.
TEST_F(GrammarImport, MissingImportIsCleanError) {
    hs_database_t *db = nullptr;
    EXPECT_EQ(HS_COMPILER_ERROR, compileHsg("use_missing.hsg", &db));
    EXPECT_EQ(nullptr, db);
}

// An import cycle is detected and reported cleanly — no hang, no crash.
TEST_F(GrammarImport, CycleDetected) {
    hs_database_t *db = nullptr;
    EXPECT_EQ(HS_COMPILER_ERROR, compileHsg("A2.hsg", &db));
    EXPECT_EQ(nullptr, db);
}

// Imports resolve recursively through a genuine 4-level chain.
TEST_F(GrammarImport, MultiLevelChain) {
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, compileHsg("L1.hsg", &db));
    const std::string corpus = "level1 level2 level3 level4";
    EXPECT_TRUE(scanHas(db, corpus, "level1"));
    EXPECT_TRUE(scanHas(db, corpus, "level4"));   // deepest file, reached via 3 imports
    hs_free_database(db);
}

// Letter flags map to real HS_FLAG bits: 'L' = SOM_LEFTMOST changes the reported start offset.
TEST_F(GrammarImport, FlagLetterSom) {
    hs_database_t *db = nullptr;
    ASSERT_EQ(HS_SUCCESS, compileHsg("som.hsg", &db));
    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(HS_SUCCESS, hs_alloc_scratch(db, &scratch));
    // record (id -> start offset) for the match
    struct S { unsigned long long fromFor700 = ~0ull, fromFor701 = ~0ull; } s;
    auto cb = [](unsigned id, unsigned long long from, unsigned long long, unsigned, void *c) -> int {
        auto *p = static_cast<S *>(c);
        if (id == 700) p->fromFor700 = from;
        if (id == 701) p->fromFor701 = from;
        return 0;
    };
    ASSERT_EQ(HS_SUCCESS, hs_scan(db, "zzzz Zeta", 9, 0, scratch, cb, &s));
    EXPECT_EQ(5u, s.fromFor700);   // /L -> SOM on -> real start offset
    EXPECT_EQ(0u, s.fromFor701);   // no flag -> start not tracked -> 0
    hs_free_scratch(scratch);
    hs_free_database(db);
}

} // namespace
