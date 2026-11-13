#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/checkers/TypeConversionChecker.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
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
/**
 * @brief Analyzes an AngelScript snippet and returns semantic diagnostics.
 * @param[in] code Source code snippet to analyze.
 * @param[in] fileUri Virtual document URI for symbol collection and diagnostic reporting.
 * @return Vector of diagnostics generated during semantic analysis.
 */
std::vector<Diagnostic> AnalyzeCode(const std::string& code,
                                    const std::string& fileUri = "file:///ternary_conversion_variations.as")
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

/**
 * @brief Checks if any emitted diagnostic is a type conversion error.
 * @param[in] diagnostics Emitted diagnostic list.
 * @return True if a type conversion diagnostic is present; false otherwise.
 */
bool HasConversionDiagnostic(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& d) { return d.code == "as-err-no-implicit-conversion"; });
}

/**
 * @brief Matches emitted diagnostics against the expected verdict.
 * @param[in] diagnostics Emitted diagnostic list.
 * @param[in] expectedVerdict Expected diagnostic code or "none".
 * @return True if diagnostics match the expected verdict; false otherwise.
 */
bool MatchesVerdict(const std::vector<Diagnostic>& diagnostics, const std::string& expectedVerdict)
{
    if (expectedVerdict == "none")
    {
        return !HasConversionDiagnostic(diagnostics);
    }
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const Diagnostic& d) { return d.code == expectedVerdict; });
}
} // namespace

TEST_SUITE("TernaryConversionVariations")
{
    TEST_CASE("Variation 01 - Handle consequence and handle alternative of same type")
    {
        const std::string classSymbol = angel_lsp::test::GenerateRandomSymbolName("Widget");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("ProcessWidgets");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string primaryParam = angel_lsp::test::GenerateRandomSymbolName("primary");
        const std::string fallbackParam = angel_lsp::test::GenerateRandomSymbolName("fallback");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code = "class " + classSymbol +
                                 "\n"
                                 "{\n"
                                 "}\n"
                                 "void " +
                                 testFn + "(bool " + flagParam + ", " + classSymbol + "@ " + primaryParam + ", " +
                                 classSymbol + "@ " + fallbackParam +
                                 ")\n"
                                 "{\n"
                                 "    " +
                                 classSymbol + "@ " + selectedVar + " = " + flagParam + " ? " + primaryParam + " : " +
                                 fallbackParam +
                                 ";\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasConversionDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 02 - Handle consequence and null alternative")
    {
        const std::string classSymbol = angel_lsp::test::GenerateRandomSymbolName("Resource");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("AcquireResource");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string itemParam = angel_lsp::test::GenerateRandomSymbolName("item");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code = "class " + classSymbol +
                                 "\n"
                                 "{\n"
                                 "}\n"
                                 "void " +
                                 testFn + "(bool " + flagParam + ", " + classSymbol + "@ " + itemParam +
                                 ")\n"
                                 "{\n"
                                 "    " +
                                 classSymbol + "@ " + selectedVar + " = " + flagParam + " ? " + itemParam +
                                 " : null;\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasConversionDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 03 - Null consequence and handle alternative")
    {
        const std::string classSymbol = angel_lsp::test::GenerateRandomSymbolName("Session");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("InitSession");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string activeParam = angel_lsp::test::GenerateRandomSymbolName("active");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code = "class " + classSymbol +
                                 "\n"
                                 "{\n"
                                 "}\n"
                                 "void " +
                                 testFn + "(bool " + flagParam + ", " + classSymbol + "@ " + activeParam +
                                 ")\n"
                                 "{\n"
                                 "    " +
                                 classSymbol + "@ " + selectedVar + " = " + flagParam + " ? null : " + activeParam +
                                 ";\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasConversionDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 07 - String consequence and string alternative")
    {
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("FormatMessage");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string prefixParam = angel_lsp::test::GenerateRandomSymbolName("prefix");
        const std::string fallbackParam = angel_lsp::test::GenerateRandomSymbolName("fallback");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code = "void " + testFn + "(bool " + flagParam + ", string " + prefixParam + ", string " +
                                 fallbackParam +
                                 ")\n"
                                 "{\n"
                                 "    string " +
                                 selectedVar + " = " + flagParam + " ? " + prefixParam + " : " + fallbackParam +
                                 ";\n"
                                 "}\n";

        const std::string expectedVerdict = "none";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK_FALSE(HasConversionDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 08 - Incompatible handle and string branches emit error")
    {
        const std::string classSymbol = angel_lsp::test::GenerateRandomSymbolName("Document");
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("PrintSummary");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string docParam = angel_lsp::test::GenerateRandomSymbolName("doc");
        const std::string fallbackParam = angel_lsp::test::GenerateRandomSymbolName("fallback");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code =
            "class " + classSymbol +
            "\n"
            "{\n"
            "}\n"
            "void " +
            testFn + "(bool " + flagParam + ", " + classSymbol + "@ " + docParam + ", string " + fallbackParam +
            ")\n"
            "{\n"
            "    " +
            classSymbol + "@ " + selectedVar + " = " + flagParam + " ? " + docParam + " : " + fallbackParam +
            ";\n"
            "}\n";

        const std::string expectedVerdict = "as-err-no-implicit-conversion";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasConversionDiagnostic(diagnostics));
    }

    TEST_CASE("Variation 09 - Incompatible string and null branches emit error")
    {
        const std::string testFn = angel_lsp::test::GenerateRandomSymbolName("ReadHeader");
        const std::string flagParam = angel_lsp::test::GenerateRandomSymbolName("flag");
        const std::string headerParam = angel_lsp::test::GenerateRandomSymbolName("header");
        const std::string selectedVar = angel_lsp::test::GenerateRandomSymbolName("selected");

        const std::string code = "void " + testFn + "(bool " + flagParam + ", string " + headerParam +
                                 ")\n"
                                 "{\n"
                                 "    string " +
                                 selectedVar + " = " + flagParam + " ? " + headerParam +
                                 " : null;\n"
                                 "}\n";

        const std::string expectedVerdict = "as-err-no-implicit-conversion";
        const auto diagnostics = AnalyzeCode(code);
        CHECK(MatchesVerdict(diagnostics, expectedVerdict));
        CHECK(HasConversionDiagnostic(diagnostics));
    }
}
