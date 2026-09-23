/**
 * @file PrimitiveVsCustomClassCastTest.cpp
 * @brief Verification suite for scalar primitive casts vs user-defined class constructor resolution.
 */

#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
namespace diag_codes = angel_lsp::diagnostics::codes;

namespace
{
/**
 * @brief Analyzes a code snippet through the full semantic analysis pipeline.
 *
 * @param[in] code AngelScript source text.
 * @param[in] fileUri Virtual document URI.
 * @return Collected diagnostics from all analysis passes.
 */
std::vector<Diagnostic> AnalyzeScript(const std::string& code, const std::string& fileUri = "file:///cast_test.as")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    static i18n::I18n i18n;

    auto diagnostics = collector.CollectSymbols({fileUri, code, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
    request.scopeRoot = scopes.CollectScopes(code, parser);
    request.sourceCode = code;
    request.tree = parser.Parse(code);

    SemanticAnalyzer analyzer(nullptr);
    auto semDiags = analyzer.Analyze(request);
    diagnostics.insert(diagnostics.end(), semDiags.begin(), semDiags.end());

    if (request.tree)
    {
        ts_tree_delete(const_cast<TSTree*>(request.tree));
    }
    return diagnostics;
}

/**
 * @brief Filters a collection of diagnostics to only include error severity.
 *
 * @param[in] diagnostics Diagnostics list.
 * @return Diagnostics with Error severity.
 */
std::vector<Diagnostic> FilterErrors(const std::vector<Diagnostic>& diagnostics)
{
    std::vector<Diagnostic> errors;
    for (const auto& d : diagnostics)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            errors.push_back(d);
        }
    }
    return errors;
}

/**
 * @brief Recursively locates all functional cast and call expression nodes in an AST.
 *
 * @param[in] node Starting node.
 * @param[out] outNodes Vector receiving matched nodes.
 */
void CollectCastAndCallNodes(TSNode node, std::vector<TSNode>& outNodes)
{
    const std::string_view nodeType(ts_node_type(node));
    if (nodeType == "functional_cast_expression" || nodeType == "call_expression")
    {
        outNodes.push_back(node);
    }

    const uint32_t count = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < count; ++i)
    {
        CollectCastAndCallNodes(ts_node_named_child(node, i), outNodes);
    }
}

/**
 * @brief Helper that checks whether as-err-no-matching-constructor is emitted for char(uint8).
 *
 * @param[in] code Source code to analyze.
 */
void AssertCharInitFails(const std::string& code)
{
    const auto diags = AnalyzeScript(code);
    const auto errors = FilterErrors(diags);
    REQUIRE_FALSE(errors.empty());

    bool foundExpectedError = false;
    for (const auto& err : errors)
    {
        if (err.code == diag_codes::NoMatchingConstructor)
        {
            foundExpectedError = true;
            CHECK(err.message.find("char(uint8)") != std::string::npos);
        }
    }
    CHECK(foundExpectedError);
}
} // namespace

TEST_SUITE_BEGIN("PrimitiveVsCustomClassCast");

/**
 * @brief Verifies that genuine AngelScript primitive functional casts produce 0 errors
 *        and evaluate strictly to their target scalar types.
 */
TEST_CASE("PrimitiveVsCustomClassCast - Genuine primitive functional casts produce 0 diagnostics and correct types")
{
    const std::string fnName = test::GenerateRandomSymbolName("TestPrimCast");
    const std::string varB = test::GenerateRandomSymbolName("b");
    const std::string varX = test::GenerateRandomSymbolName("x");
    const std::string varF = test::GenerateRandomSymbolName("f");
    const std::string varOk = test::GenerateRandomSymbolName("ok");

    const std::string code = "void " + fnName + "()\n"
                             "{\n"
                             "    uint8 " + varB + " = 65;\n"
                             "    int " + varX + " = int(" + varB + ");\n"
                             "    float " + varF + " = float(" + varB + ");\n"
                             "    bool " + varOk + " = bool(" + varB + ");\n"
                             "}\n";

    const auto diags = AnalyzeScript(code);
    CHECK(FilterErrors(diags).empty());

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    const std::string fileUri = "file:///prim_types.as";
    collector.CollectSymbols(fileUri, code, parser, table);

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    TSNode root = ts_tree_root_node(tree);
    auto scopeRoot = scopes.CollectScopesFromTree(root, code);

    std::vector<TSNode> castNodes;
    CollectCastAndCallNodes(root, castNodes);
    REQUIRE(castNodes.size() >= 3);

    const Scope* scope = FindInnermostScope(scopeRoot.get(), 3, 4);
    ExpressionTypeContext ctx{scope, table, code, fileUri};

    std::vector<std::string> evaluatedTypes;
    for (TSNode castNode : castNodes)
    {
        evaluatedTypes.push_back(ResolveExpressionType(castNode, ctx));
    }

    CHECK(std::find(evaluatedTypes.begin(), evaluatedTypes.end(), "int") != evaluatedTypes.end());
    CHECK(std::find(evaluatedTypes.begin(), evaluatedTypes.end(), "float") != evaluatedTypes.end());
    CHECK(std::find(evaluatedTypes.begin(), evaluatedTypes.end(), "bool") != evaluatedTypes.end());

    ts_tree_delete(tree);
}

/**
 * @brief Verifies that a user-defined char class matching Sven Co-op's stub without a numeric
 *        constructor emits as-err-no-matching-constructor with signature char(uint8).
 */
TEST_CASE("PrimitiveVsCustomClassCast - Custom char class without numeric constructor emits as-err-no-matching-constructor")
{
    const std::string fnName = test::GenerateRandomSymbolName("TestCharInit");
    const std::string varVal = test::GenerateRandomSymbolName("val");
    const std::string varC = test::GenerateRandomSymbolName("c");

    SUBCASE("Constructor without explicit return type")
    {
        const std::string code = "class char\n"
                                 "{\n"
                                 "    char();\n"
                                 "    char(const string& in);\n"
                                 "    char(const char& in);\n"
                                 "}\n"
                                 "void " + fnName + "()\n"
                                 "{\n"
                                 "    uint8 " + varVal + " = 65;\n"
                                 "    char " + varC + "(" + varVal + ");\n"
                                 "}\n";

        AssertCharInitFails(code);
    }

    SUBCASE("Constructor with void return type matching Sven Co-op predefined stub")
    {
        const std::string code = "class char\n"
                                 "{\n"
                                 "    void char();\n"
                                 "    void char(const string& in);\n"
                                 "    void char(const char& in);\n"
                                 "}\n"
                                 "void " + fnName + "()\n"
                                 "{\n"
                                 "    uint8 " + varVal + " = 65;\n"
                                 "    char " + varC + "(" + varVal + ");\n"
                                 "}\n";

        AssertCharInitFails(code);
    }
}

/**
 * @brief Verifies that a custom class with an integer constructor succeeds with uint8 argument
 *        via implicit widening numeric promotion, producing zero diagnostics.
 */
TEST_CASE("PrimitiveVsCustomClassCast - Custom class with integer constructor accepts uint8 via widening promotion")
{
    const std::string customClassName = test::GenerateRandomSymbolName("CustomChar");
    const std::string fnName = test::GenerateRandomSymbolName("TestCustomInit");
    const std::string varVal = test::GenerateRandomSymbolName("val");
    const std::string varC = test::GenerateRandomSymbolName("c");

    const std::string code = "class " + customClassName + "\n"
                             "{\n"
                             "    " + customClassName + "(int code);\n"
                             "}\n"
                             "void " + fnName + "()\n"
                             "{\n"
                             "    uint8 " + varVal + " = 65;\n"
                             "    " + customClassName + " " + varC + "(" + varVal + ");\n"
                             "}\n";

    const auto diags = AnalyzeScript(code);
    CHECK(FilterErrors(diags).empty());
}

TEST_SUITE_END();
