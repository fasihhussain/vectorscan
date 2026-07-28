/*
 * xml_grammar.cpp — grammar XML loading (DOCTYPE-safe) + entity extraction.
 *
 * Uses Boost.PropertyTree (already a Vectorscan dependency — see cmake/boost.cmake),
 * so no new third-party dependency is introduced. The <!DOCTYPE ...> line is stripped
 * before parsing (validated: the reader otherwise chokes on the external "edk.dtd").
 */
#include "grammar_import.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <fstream>
#include <regex>
#include <sstream>

namespace pt = boost::property_tree;

namespace ue2 {
namespace grammar {

static const std::regex RE_DOCTYPE(R"(<!DOCTYPE[^>]*>)");
static const std::regex RE_INCLUDE(R"RGX(<include\b[^>]*\bpath\s*=\s*"([^"]+)")RGX");
static const std::regex RE_GRAMMAR_NAME(R"RGX(<grammar\b[^>]*\bname\s*=\s*"([^"]+)")RGX");

std::string readStripped(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return std::regex_replace(ss.str(), RE_DOCTYPE, "");
}

bool parseGrammarXml(const std::string &xml, Registry &reg, std::string &errMsg) {
    pt::ptree tree;
    try {
        std::istringstream is(xml);
        pt::read_xml(is, tree,
                     pt::xml_parser::no_comments | pt::xml_parser::trim_whitespace);
    } catch (const std::exception &e) {
        errMsg = std::string("grammar XML parse failed: ") + e.what();
        return false;
    }

    auto grammars = tree.get_child_optional("grammars");
    if (!grammars) {
        errMsg = "grammar XML has no <grammars> root";
        return false;
    }
    for (const auto &g : *grammars) {
        if (g.first != "grammar") continue;
        const std::string gname = g.second.get<std::string>("<xmlattr>.name", "");
        auto &emap = reg[gname];
        for (const auto &e : g.second) {
            if (e.first != "entity") continue;
            const std::string ename = e.second.get<std::string>("<xmlattr>.name", "");
            if (ename.empty()) continue;
            std::vector<std::string> entries;
            for (const auto &c : e.second) {
                if (c.first == "entry") {
                    if (auto hw = c.second.get_optional<std::string>("<xmlattr>.headword")) {
                        entries.push_back(*hw);
                    }
                } else if (c.first == "pattern") {
                    entries.push_back(c.second.data());
                }
            }
            if (!emap.count(ename)) emap[ename] = std::move(entries);  // first source wins
        }
    }
    return true;
}

std::vector<std::string> parseIncludePaths(const std::string &mainXml) {
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(mainXml.begin(), mainXml.end(), RE_INCLUDE);
         it != std::sregex_iterator(); ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

std::set<std::string> grammarNames(const std::string &xml) {
    std::set<std::string> out;
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), RE_GRAMMAR_NAME);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    return out;
}

}  // namespace grammar
}  // namespace ue2
