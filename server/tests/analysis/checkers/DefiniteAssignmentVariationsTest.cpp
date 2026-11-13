#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/checkers/DefiniteAssignmentChecker.h"
#include "analysis/index/DefiniteAssignmentFacts.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
std::vector<Diagnostic> AnalyzeCode(const std::string& code,
                                    const std::string& fileUri = "file:///definite_assignment_variations.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static angel_lsp::i18n::I18n i18n;

    collector.CollectSymbols(fileUri, code, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    TSTree* tree = parser.Parse(code);
    NodeIndex nodeIndex(ts_tree_root_node(tree));
    request.nodeIndex = &nodeIndex;

    SemanticAnalyzer analyzer(nullptr);
    auto diagnostics = analyzer.Analyze(request);

    if (tree)
    {
        ts_tree_delete(tree);
    }
    return diagnostics;
}

bool HasUninitializedRead(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.code == "as-warn-uninitialized-variable-read"; });
}

bool MatchesVerdict(const std::vector<Diagnostic>& diagnostics, const std::string& expectedVerdict)
{
    if (expectedVerdict == "none")
    {
        return !HasUninitializedRead(diagnostics);
    }
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == expectedVerdict; });
}
} // namespace

TEST_SUITE("DefiniteAssignmentVariations")
{
    TEST_CASE("Variation 01 - Out param assigned in nested if within for loop read after loop")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 42; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 5; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        if (" +
                                 iVar +
                                 " == 2)\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 02 - Out param read inside nested if within for loop before assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 42; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 5; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        if (" +
                                 iVar +
                                 " == 2)\n"
                                 "        {\n"
                                 "            " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 04 - Out param assigned in nested for loops in both if and else branches read after")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string jVar = angel_lsp::test::GenerateRandomSymbolName("j");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 100; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(bool " + flagParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    if (" +
                                 flagParam +
                                 ")\n"
                                 "    {\n"
                                 "        for (int " +
                                 iVar + " = 0; " + iVar + " < 2; ++" + iVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    else\n"
                                 "    {\n"
                                 "        for (int " +
                                 jVar + " = 0; " + jVar + " < 3; ++" + jVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "    " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 06 - Out param assigned in nested do-while within if block read after do-while")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string condParam = angel_lsp::test::GenerateRandomSymbolName("cond");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 99; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(bool " + condParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    if (" +
                                 condParam +
                                 ")\n"
                                 "    {\n"
                                 "        do\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        } while (false);\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 07 - Out param read in for loop condition inside switch case before body assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string optionParam = angel_lsp::test::GenerateRandomSymbolName("option");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 1; }\n"
                                 "void " +
                                 testFn + "(int " + optionParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    switch (" +
                                 optionParam +
                                 ")\n"
                                 "    {\n"
                                 "    case 1:\n"
                                 "        for (int " +
                                 iVar + " = 0; " + iVar + " < " + dataVar + "; ++" + iVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "        break;\n"
                                 "    default:\n"
                                 "        break;\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 08 - Out param assigned in while condition expression read inside nested if")
    {
        const std::string tryFetchFn = angel_lsp::test::GenerateRandomSymbolName("TryFetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "bool " + tryFetchFn + "(int &out " + targetParam +
                                 ")\n"
                                 "{\n"
                                 "    " +
                                 targetParam +
                                 " = 42;\n"
                                 "    return true;\n"
                                 "}\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    while (" +
                                 tryFetchFn + "(" + dataVar +
                                 "))\n"
                                 "    {\n"
                                 "        if (" +
                                 dataVar +
                                 " > 0)\n"
                                 "        {\n"
                                 "            " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "        break;\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 09 - Out param read in outer for loop before nested inner for loop assignment")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string iVar = angel_lsp::test::GenerateRandomSymbolName("i");
        const std::string jVar = angel_lsp::test::GenerateRandomSymbolName("j");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 5; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn +
                                 "()\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    for (int " +
                                 iVar + " = 0; " + iVar + " < 3; ++" + iVar +
                                 ")\n"
                                 "    {\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "        for (int " +
                                 jVar + " = 0; " + jVar + " < 3; ++" + jVar +
                                 ")\n"
                                 "        {\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "        }\n"
                                 "    }\n"
                                 "}\n";

        const std::string expectedVerdict = "as-warn-uninitialized-variable-read";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasUninitializedRead(diagnostics));
    }

    TEST_CASE("Variation 10 - Out param assigned across all switch cases inside do-while read after switch")
    {
        const std::string fetchFn = angel_lsp::test::GenerateRandomSymbolName("Fetch");
        const std::string consumeFn = angel_lsp::test::GenerateRandomSymbolName("Consume");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("TestFunc");
        const std::string dataVar = angel_lsp::test::GenerateRandomSymbolName("data");
        const std::string tagParam = angel_lsp::test::GenerateRandomSymbolName("tag");
        const std::string targetParam = angel_lsp::test::GenerateRandomSymbolName("target");
        const std::string valueParam = angel_lsp::test::GenerateRandomSymbolName("value");

        const std::string code = "void " + fetchFn + "(int &out " + targetParam + ") { " + targetParam +
                                 " = 12; }\n"
                                 "void " +
                                 consumeFn + "(int " + valueParam +
                                 ") { }\n"
                                 "void " +
                                 testFn + "(int " + tagParam +
                                 ")\n"
                                 "{\n"
                                 "    int " +
                                 dataVar +
                                 ";\n"
                                 "    do\n"
                                 "    {\n"
                                 "        switch (" +
                                 tagParam +
                                 ")\n"
                                 "        {\n"
                                 "        case 1:\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "            break;\n"
                                 "        default:\n"
                                 "            " +
                                 fetchFn + "(" + dataVar +
                                 ");\n"
                                 "            break;\n"
                                 "        }\n"
                                 "        " +
                                 consumeFn + "(" + dataVar +
                                 ");\n"
                                 "    } while (false);\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasUninitializedRead(diagnostics));
    }
}
