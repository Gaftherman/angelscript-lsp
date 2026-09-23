/**
 * @file PrimitiveCastTest.cpp
 * @brief Unit tests for constructor-style primitive casting and contextual keyword usage.
 */

#include "analysis/CallChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_SUITE("PrimitiveCastAndContextualKeywords")
{
    /**
     * @brief Verifies that constructor-style primitive casting like int(uint8) resolves type correctly.
     */
    TEST_CASE("PrimitiveCast - Resolves constructor-style primitive casts")
    {
        const std::string funcName = test::GenerateRandomSymbolName("CastFunc");
        const std::string code = "void " + funcName + "(uint8 val)\n" +
                                 "{\n" +
                                 "    int(val);\n" +
                                 "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///prim_cast.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        TSNode callNode{};
        auto search = [&callNode](TSNode node, auto& self) -> void
        {
            const std::string_view nodeType(ts_node_type(node));
            if (nodeType == "functional_cast_expression" || nodeType == "call_expression")
            {
                callNode = node;
                return;
            }
            uint32_t count = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                self(ts_node_named_child(node, i), self);
                if (!ts_node_is_null(callNode))
                    return;
            }
        };
        search(root, search);

        REQUIRE(!ts_node_is_null(callNode));

        const TSPoint pt = ts_node_start_point(callNode);
        const Scope* scope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);

        ExpressionTypeContext ctx{scope, table, code, fileUri};
        std::string exprType = ResolveExpressionType(callNode, ctx);

        CHECK(exprType == "int");

        ts_tree_delete(tree);
    }

    /**
     * @brief Verifies direct initialization where variable name matches its class name.
     */
    TEST_CASE("PrimitiveCast - Direct initialization when variable name matches class name")
    {
        const std::string className = test::GenerateRandomSymbolName("Logger");

        const std::string code = "class " + className + "\n" +
                                 "{\n" +
                                 "    " + className + "(int priority) {}\n" +
                                 "}\n" +
                                 className + " global" + className + "(10);\n" +
                                 "void Test()\n" +
                                 "{\n" +
                                 "    " + className + " " + className + "(5);\n" +
                                 "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static i18n::I18n i18n;

        const std::string fileUri = "file:///logger_init.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = std::move(scopeRoot);
        request.sourceCode = code;
        request.tree = tree;

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);

        // Should not have any "no matching constructor" errors
        for (const auto& d : diags)
        {
            CHECK(d.code != "as-err-no-matching-constructor");
            CHECK(d.code != "as-err-call-argument-count");
        }

        // Verify that CallReference was emitted for the constructor call inside Test()
        auto callRefs = table.FindSymbols(className);
        bool hasCallRef = false;
        for (const auto& sym : callRefs)
        {
            if (sym.type == SymbolType::CallReference)
            {
                hasCallRef = true;
                break;
            }
        }
        CHECK(hasCallRef);

        ts_tree_delete(tree);
    }

    /**
     * @brief Verifies that contextual keywords like 'function' can be used as variables and in comparisons.
     */
    TEST_CASE("PrimitiveCast - Contextual keyword 'function' in handle comparison")
    {
        const std::string cbName = test::GenerateRandomSymbolName("Callback");

        const std::string code = "funcdef void " + cbName + "();\n" +
                                 "void Process(" + cbName + "@ function)\n" +
                                 "{\n" +
                                 "    if (function !is null)\n" +
                                 "    {\n" +
                                 "    }\n" +
                                 "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static i18n::I18n i18n;

        const std::string fileUri = "file:///contextual_func.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        SemanticAnalysisRequest request{table, fileUri, ".as.predefined", &i18n};
        request.scopeRoot = std::move(scopeRoot);
        request.sourceCode = code;
        request.tree = tree;

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);

        // Zero errors expected
        for (const auto& d : diags)
        {
            CHECK(d.severity != DiagnosticSeverity::Error);
        }

        ts_tree_delete(tree);
    }
}
