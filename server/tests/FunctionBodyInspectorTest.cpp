#include <doctest/doctest.h>

#include "analysis/FunctionBodyInspector.h"
#include "parser/AngelScriptParser.h"
#include <tree_sitter/api.h>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("FunctionBodyInspector - Direct AST Inspection")
{
    AngelScriptParser parser;

    SUBCASE("Collects identifier references and distinguishes member access")
    {
        std::string sourceCode = "void main() { int a = 10; foo(a); obj.bar(); }\n";
        TSTree *tree = parser.Parse(sourceCode);
        REQUIRE(tree != nullptr);
        TSNode rootNode = ts_tree_root_node(tree);

        TSNode bodyNode = ts_node_child(rootNode, 0);
        uint32_t cCount = ts_node_child_count(bodyNode);
        TSNode blockNode{};
        for (uint32_t i = 0; i < cCount; ++i)
        {
            TSNode ch = ts_node_child(bodyNode, i);
            if (std::string(ts_node_type(ch)) == "statement_block")
            {
                blockNode = ch;
                break;
            }
        }

        FunctionBodyAnalysis analysis;
        if (!ts_node_is_null(blockNode))
        {
            InspectFunctionBodyAST(blockNode, analysis, sourceCode);
        }

        CHECK(!analysis.bodyIdentifierRefs.empty());

        bool foundFooCall = false;
        bool foundObjRef = false;
        bool foundBarMember = false;

        for (const auto &ref : analysis.bodyIdentifierRefs)
        {
            if (ref.name == "foo" && ref.isCall) foundFooCall = true;
            if (ref.name == "obj" && !ref.isMemberAccess) foundObjRef = true;
            if (ref.name == "bar" && ref.isMemberAccess) foundBarMember = true;
        }

        CHECK(foundFooCall);
        CHECK(foundObjRef);
        CHECK(foundBarMember);

        ts_tree_delete(tree);
    }

    SUBCASE("Detects invalid break and continue statements outside loops")
    {
        std::string sourceCode = "void main() { break; continue; }\n";
        TSTree *tree = parser.Parse(sourceCode);
        REQUIRE(tree != nullptr);
        TSNode rootNode = ts_tree_root_node(tree);

        TSNode bodyNode = ts_node_child(rootNode, 0);
        uint32_t cCount = ts_node_child_count(bodyNode);
        TSNode blockNode{};
        for (uint32_t i = 0; i < cCount; ++i)
        {
            TSNode ch = ts_node_child(bodyNode, i);
            if (std::string(ts_node_type(ch)) == "statement_block")
            {
                blockNode = ch;
                break;
            }
        }

        FunctionBodyAnalysis analysis;
        if (!ts_node_is_null(blockNode))
        {
            InspectFunctionBodyAST(blockNode, analysis, sourceCode);
        }

        CHECK(analysis.invalidBreakStatements.size() == 1);
        CHECK(analysis.invalidContinueStatements.size() == 1);

        ts_tree_delete(tree);
    }
}
