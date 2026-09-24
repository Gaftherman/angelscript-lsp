#include <doctest/doctest.h>

#include "analysis/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "helpers/TestUtils.h"

#include <string>
#include <vector>

namespace
{

/**
 * @brief Traverses AST using a single flat cursor to locate the first argument_list node.
 * @param[in] tree Owning Tree-sitter tree.
 * @return argument_list TSNode or null node if not found.
 */
TSNode FindFirstArgumentList(TSTree* tree)
{
    if (!tree)
    {
        return TSNode{};
    }

    TSNode root = ts_tree_root_node(tree);
    TSTreeCursor cursor = ts_tree_cursor_new(root);

    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (std::string_view(ts_node_type(node)) == "argument_list")
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

TEST_CASE("AST Structural Traversal - Multi-Argument Template Types")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("func");
    const std::string keyType = angel_lsp::test::GenerateRandomSymbolName("Key");
    const std::string valType = angel_lsp::test::GenerateRandomSymbolName("Val");

    const std::string code =
        "void " + fnName + "(dictionary<" + keyType + ", " + valType + "> dict, int val) {}\n"
        "void Run() {\n"
        "    " + fnName + "(dictionary<" + keyType + ", " + valType + ">(), 42);\n"
        "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode argList = FindFirstArgumentList(tree);
    REQUIRE(!ts_node_is_null(argList));

    size_t count = angel_lsp::analysis::CountCallArguments(argList);
    CHECK(count == 2);

    auto args = angel_lsp::analysis::ExtractCallArguments(argList, code);
    REQUIRE(args.size() == 2);
    CHECK(args[0].index == 0);
    CHECK(args[1].index == 1);
    CHECK(args[0].name.empty());
    CHECK(args[1].name.empty());

    ts_tree_delete(tree);
}

TEST_CASE("AST Structural Traversal - Default Arguments with Nested Calls and Strings")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string outerFn = angel_lsp::test::GenerateRandomSymbolName("Outer");
    const std::string innerFn = angel_lsp::test::GenerateRandomSymbolName("Inner");

    const std::string code =
        "void Run() {\n"
        "    " + outerFn + "(" + innerFn + "(\"arg1, with, commas\", 10, 20), /* comment, 1 */ 99);\n"
        "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode argList = FindFirstArgumentList(tree);
    REQUIRE(!ts_node_is_null(argList));

    // Outer call should have exactly 2 arguments
    size_t count = angel_lsp::analysis::CountCallArguments(argList);
    CHECK(count == 2);

    auto args = angel_lsp::analysis::ExtractCallArguments(argList, code);
    REQUIRE(args.size() == 2);
    CHECK(args[0].index == 0);
    CHECK(args[1].index == 1);

    ts_tree_delete(tree);
}

TEST_CASE("AST Structural Traversal - Named Arguments Mapping")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Process");
    const std::string arg1Name = angel_lsp::test::GenerateRandomSymbolName("first");
    const std::string arg2Name = angel_lsp::test::GenerateRandomSymbolName("second");

    const std::string code =
        "void Run() {\n"
        "    " + fnName + "(" + arg1Name + ": 100, " + arg2Name + ": 200);\n"
        "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode argList = FindFirstArgumentList(tree);
    REQUIRE(!ts_node_is_null(argList));

    size_t count = angel_lsp::analysis::CountCallArguments(argList);
    CHECK(count == 2);

    auto args = angel_lsp::analysis::ExtractCallArguments(argList, code);
    REQUIRE(args.size() == 2);
    CHECK(args[0].index == 0);
    CHECK(args[0].name == arg1Name);
    CHECK(!ts_node_is_null(args[0].nameNode));
    CHECK(!ts_node_is_null(args[0].exprNode));

    CHECK(args[1].index == 1);
    CHECK(args[1].name == arg2Name);
    CHECK(!ts_node_is_null(args[1].nameNode));
    CHECK(!ts_node_is_null(args[1].exprNode));

    ts_tree_delete(tree);
}

TEST_CASE("AST Structural Traversal - Malformed or Incomplete Argument Lists")
{
    angel_lsp::parser::AngelScriptParser parser;
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Incomplete");

    // Syntax error with double comma
    const std::string code =
        "void Run() {\n"
        "    " + fnName + "(10, , 20);\n"
        "}\n";

    TSTree* tree = parser.Parse(code);
    REQUIRE(tree != nullptr);
    TSNode argList = FindFirstArgumentList(tree);
    REQUIRE(!ts_node_is_null(argList));

    // Safe extraction without crash or out-of-bounds
    auto args = angel_lsp::analysis::ExtractCallArguments(argList, code);
    CHECK(!args.empty());

    // Active parameter resolution during live typing
    const size_t posBeforeFirstComma = code.find(fnName) + fnName.size() + 2; // right after "10"
    const size_t posAfterFirstComma = code.find(',') + 1;
    const size_t posAfterSecondComma = code.rfind(',') + 1;

    CHECK(angel_lsp::analysis::CalculateActiveCallParameter(argList, posBeforeFirstComma, code) == 0);
    CHECK(angel_lsp::analysis::CalculateActiveCallParameter(argList, posAfterFirstComma, code) == 1);
    CHECK(angel_lsp::analysis::CalculateActiveCallParameter(argList, posAfterSecondComma, code) == 2);

    ts_tree_delete(tree);
}
