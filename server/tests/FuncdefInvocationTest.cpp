/**
 * @file FuncdefInvocationTest.cpp
 * @brief Unit tests for return type resolution of funcdef invocations.
 */

#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_SUITE("FuncdefInvocation")
{
    /**
     * @brief Verifies that calling a funcdef handle resolves to the funcdef's return type.
     */
    TEST_CASE("FuncdefInvocation - Resolves return type of local funcdef variable invocation")
    {
        const std::string funcdefName = test::GenerateRandomSymbolName("OnAction");
        const std::string varName = test::GenerateRandomSymbolName("actionCallback");

        const std::string code = "funcdef int " + funcdefName + "(float x);\n" +
                                 "void Execute(" + funcdefName + "@ " + varName + ")\n" +
                                 "{\n" +
                                 "    " + varName + "(1.5f);\n" +
                                 "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///funcdef_invocation.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        // Find the call expression node for actionCallback(1.5f)
        TSNode callNode{};
        auto search = [&callNode, &varName, &code](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "call_expression")
            {
                TSNode fnChild = parser::GetChildByField(node, parser::fields::Function);
                if (!ts_node_is_null(fnChild))
                {
                    const uint32_t sb = ts_node_start_byte(fnChild);
                    const uint32_t eb = ts_node_end_byte(fnChild);
                    std::string text = code.substr(sb, eb - sb);
                    if (text == varName)
                    {
                        callNode = node;
                        return;
                    }
                }
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
     * @brief Verifies that calling a funcdef member property resolves to the funcdef's return type.
     */
    TEST_CASE("FuncdefInvocation - Resolves return type of member funcdef invocation")
    {
        const std::string funcdefName = test::GenerateRandomSymbolName("FilterFunc");
        const std::string className = test::GenerateRandomSymbolName("Processor");
        const std::string memberName = test::GenerateRandomSymbolName("filter");

        const std::string code = "funcdef string " + funcdefName + "(int a, int b);\n" +
                                 "class " + className + "\n" +
                                 "{\n" +
                                 "    " + funcdefName + "@ " + memberName + ";\n" +
                                 "}\n" +
                                 "void Run(" + className + "@ p)\n" +
                                 "{\n" +
                                 "    p." + memberName + "(10, 20);\n" +
                                 "}\n";

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///funcdef_member.as";
        collector.CollectSymbols(fileUri, code, parser, table);

        TSTree* tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        TSNode root = ts_tree_root_node(tree);
        auto scopeRoot = scopes.CollectScopesFromTree(root, code);

        TSNode callNode{};
        auto search = [&callNode](TSNode node, auto& self) -> void
        {
            if (std::string_view(ts_node_type(node)) == "call_expression")
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

        CHECK(exprType == "string");

        ts_tree_delete(tree);
    }
}
