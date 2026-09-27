#include "analysis/ast/SemanticNodes.h"
#include "document/Document.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::parser;
using namespace angel_lsp::analysis;
using namespace angel_lsp::analysis::ast;

namespace
{
TSNode FindNodeByType(TSNode root, std::string_view targetType)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (std::string_view(ts_node_type(node)) == targetType)
        {
            ts_tree_cursor_delete(&cursor);
            return node;
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }

        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool climbed = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                climbed = true;
                break;
            }
        }

        if (!climbed)
        {
            break;
        }
    }
    ts_tree_cursor_delete(&cursor);
    return TSNode{};
}
} // namespace

TEST_SUITE("SemanticASTFacade")
{
    TEST_CASE("AstNodeView and IfStatementView extract condition and branches with randomized identifiers")
    {
        AngelScriptParser asParser(nullptr);
        const std::string funcName = test::GenerateRandomSymbolName("func");
        const std::string varName = test::GenerateRandomSymbolName("cond");

        const std::string code = "int " + funcName + "(bool " + varName +
                                 ") {\n"
                                 "    if (" +
                                 varName +
                                 ") {\n"
                                 "        return 1;\n"
                                 "    } else {\n"
                                 "        return 2;\n"
                                 "    }\n"
                                 "}\n";

        document::TreePtr tree = document::MakeTreePtr(asParser.Parse(code));
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree.get());

        TSNode ifNode = FindNodeByType(root, "if_statement");
        REQUIRE(!ts_node_is_null(ifNode));

        IfStatementView ifView(ifNode, code);
        CHECK(ifView.IsValid());
        CHECK(ifView.Type() == "if_statement");
        CHECK(!ts_node_is_null(ifView.Condition()));
        CHECK(ifView.HasAlternative());
        CHECK(!ts_node_is_null(ifView.Alternative()));
        CHECK(!ts_node_is_null(ifView.Consequence()));
    }

    TEST_CASE("ReturnStatementView and CallExpressionView extract operands correctly")
    {
        AngelScriptParser asParser(nullptr);
        const std::string callerName = test::GenerateRandomSymbolName("caller");
        const std::string calleeName = test::GenerateRandomSymbolName("callee");
        const std::string arg1 = test::GenerateRandomSymbolName("argA");
        const std::string arg2 = test::GenerateRandomSymbolName("argB");

        const std::string code = "void " + callerName +
                                 "() {\n"
                                 "    " +
                                 calleeName + "(" + arg1 + ", " + arg2 +
                                 ");\n"
                                 "    return;\n"
                                 "}\n";

        document::TreePtr tree = document::MakeTreePtr(asParser.Parse(code));
        REQUIRE(tree != nullptr);
        TSNode root = ts_tree_root_node(tree.get());

        TSNode callNode = FindNodeByType(root, "call_expression");
        REQUIRE(!ts_node_is_null(callNode));

        CallExpressionView callView(callNode, code);
        CHECK(callView.IsValid());
        CHECK(callView.CalleeName() == calleeName);
        CHECK(callView.ArgumentCount() == 2);
        CHECK(callView.ArgumentNodes().size() == 2);

        TSNode retNode = FindNodeByType(root, "return_statement");
        REQUIRE(!ts_node_is_null(retNode));

        ReturnStatementView retView(retNode, code);
        CHECK(retView.IsValid());
        CHECK(!retView.HasValue());
    }
}
