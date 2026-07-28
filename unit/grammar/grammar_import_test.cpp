/*
 * grammar_import_test.cpp — unit + end-to-end tests for native grammar import.
 *
 * ALL grammar data here is SYNTHETIC (fake surnames in embedded strings / the fixtures/
 * folder) — the real name_grammars.zip / broad_list_name data is NEVER shipped in tests.
 *
 * Covers: DOCTYPE-safe load, cross-reference resolution + grammar-name disambiguation,
 * composite/nested-reference rejection, and the end-to-end public API
 * (hs_compile_from_grammar_files -> hs_scan -> detect) on a synthetic grammar.
 */
#include "grammar/grammar_import.h"
#include "hs.h"
#include "gtest/gtest.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace ue2::grammar;

namespace {

// ---- synthetic grammar strings (DOCTYPE included, to exercise the strip) ----
const char *const LASTNAME_XML =
    "<?xml version=\"1.0\"?>\n<!DOCTYPE grammars SYSTEM \"edk.dtd\">\n"
    "<grammars><grammar name=\"lastname\">"
    "<entity name=\"surnames\" type=\"public\">"
    "<entry headword=\"Aardvark\"/><entry headword=\"Bumblewort\"/><entry headword=\"Cricketsnap\"/>"
    "</entity></grammar></grammars>";

const char *const RARE_XML =   // a second grammar that ALSO defines "surnames" — disambiguation
    "<?xml version=\"1.0\"?>\n<!DOCTYPE grammars SYSTEM \"edk.dtd\">\n"
    "<grammars><grammar name=\"rare\">"
    "<entity name=\"surnames\" type=\"public\"><entry headword=\"Zzyzx\"/></entity>"
    "</grammar></grammars>";

const char *const FULLNAME_XML =
    "<?xml version=\"1.0\"?>\n<!DOCTYPE grammars SYSTEM \"edk.dtd\">\n"
    "<grammars><grammar name=\"fullname\">"
    "<entity name=\"composite_demo\"><pattern>(?A^lastname/surnames)</pattern></entity>"
    "</grammar></grammars>";

Registry buildFrom(std::initializer_list<const char *> xmls) {
    Registry reg;
    std::string err;
    for (const char *x : xmls) {
        // note: parseGrammarXml expects DOCTYPE-stripped input; readStripped mirrors the
        // production path, so strip via the same regex the loader uses.
        std::string stripped = std::regex_replace(std::string(x),
                                                   std::regex("<!DOCTYPE[^>]*>"), "");
        EXPECT_TRUE(parseGrammarXml(stripped, reg, err)) << err;
    }
    return reg;
}

std::string writeTemp(const std::string &name, const char *content) {
    std::string path = std::string(testing::TempDir()) + "/" + name;
    std::ofstream(path) << content;
    return path;
}

}  // namespace

TEST(GrammarImport, DoctypeIsStrippedAndParses) {
    // readStripped removes the external-DTD DOCTYPE the XML reader would choke on
    std::string path = writeTemp("gi_last.xml", LASTNAME_XML);
    std::string xml = readStripped(path);
    EXPECT_EQ(xml.find("DOCTYPE"), std::string::npos);
    Registry reg; std::string err;
    ASSERT_TRUE(parseGrammarXml(xml, reg, err)) << err;
    ASSERT_TRUE(reg.count("lastname") && reg.at("lastname").count("surnames"));
    EXPECT_EQ(reg.at("lastname").at("surnames").size(), 3u);
}

TEST(GrammarImport, ResolveCrossReference) {
    Registry reg = buildFrom({LASTNAME_XML});
    ResolveResult r = resolveReference("(?A^lastname/surnames)", reg, {});
    ASSERT_TRUE(r.ok) << r.status;
    EXPECT_EQ(r.grammar, "lastname");
    EXPECT_EQ(r.entries.size(), 3u);
}

TEST(GrammarImport, GrammarNameDisambiguatesSharedEntity) {
    Registry reg = buildFrom({LASTNAME_XML, RARE_XML});   // both define "surnames"
    EXPECT_EQ(resolveReference("(?A^lastname/surnames)", reg, {}).entries.size(), 3u);
    EXPECT_EQ(resolveReference("(?A^rare/surnames)", reg, {}).entries.size(), 1u);
}

TEST(GrammarImport, RejectsCompositeEntity) {
    Registry reg = buildFrom({FULLNAME_XML, LASTNAME_XML});
    ResolveResult r = resolveReference("(?A:composite_demo)", reg, {"fullname"});
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.status.find("COMPOSITE"), std::string::npos);
}

TEST(GrammarImport, NotFoundReference) {
    Registry reg = buildFrom({LASTNAME_XML});
    EXPECT_FALSE(resolveReference("(?A^lastname/nope)", reg, {}).ok);
}

// End-to-end: the public API compiles a DB from grammar files and it actually matches.
TEST(GrammarImport, EndToEndCompileAndScan) {
    writeTemp("lastname.xml", LASTNAME_XML);                 // include target (.xml fallback)
    std::string full = writeTemp("fullname.xml",
        "<?xml version=\"1.0\"?>\n<!DOCTYPE grammars SYSTEM \"edk.dtd\">\n"
        "<grammars><include path=\"lastname.ecr\"/>"
        "<grammar name=\"fullname\"><entity name=\"x\"><pattern>placeholder</pattern></entity></grammar>"
        "</grammars>");

    const char *paths[] = {full.c_str()};
    hs_database_t *db = nullptr;
    hs_compile_error_t *err = nullptr;
    hs_error_t rc = hs_compile_from_grammar_files(paths, 1, "(?A^lastname/surnames)",
                                                  HS_FLAG_SOM_LEFTMOST, HS_MODE_BLOCK,
                                                  nullptr, &db, &err);
    ASSERT_EQ(rc, HS_SUCCESS) << (err && err->message ? err->message : "?");
    ASSERT_NE(db, nullptr);

    hs_scratch_t *scratch = nullptr;
    ASSERT_EQ(hs_alloc_scratch(db, &scratch), HS_SUCCESS);

    struct Cap { int hits = 0; } cap;
    auto onMatch = [](unsigned, unsigned long long, unsigned long long,
                      unsigned, void *ctx) -> int { static_cast<Cap *>(ctx)->hits++; return 0; };

    const char *corpus = "the witness Bumblewort testified";   // contains a synthetic surname
    ASSERT_EQ(hs_scan(db, corpus, (unsigned)strlen(corpus), 0, scratch, onMatch, &cap), HS_SUCCESS);
    EXPECT_GT(cap.hits, 0);                                    // "Bumblewort" detected

    hs_free_scratch(scratch);
    hs_free_database(db);
}
