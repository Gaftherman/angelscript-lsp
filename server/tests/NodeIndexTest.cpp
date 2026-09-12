#include <doctest/doctest.h>
#include "helpers/TestUtils.h"
#include "analysis/NodeIndex.h"
#include "parser/GrammarNames.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("NodeIndex - Empty on null root")
{
    NodeIndex index;
    CHECK(index.Empty());
    CHECK(index.NodeCount() == 0);
    CHECK(index.Nodes("call_expression").empty());
    CHECK(index.AllNodes().empty());
}

TEST_CASE("NodeIndex - Indexes nodes in document order")
{
    const std::string code = R"(
        void foo()
        {
            int a = 1;
            int b = 2;
            bar(a, b);
        }

        void bar(int x, int y)
        {
            foo();
        }
    )";

    auto doc = angel_lsp::test::CreateTestDocument("file:///test_node_index.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    NodeIndex index(ts_tree_root_node(doc->GetTree()));
    CHECK(!index.Empty());
    CHECK(index.NodeCount() > 0);

    auto funcDecls = index.Nodes(nodes::FuncDeclaration);
    CHECK(funcDecls.size() == 2);

    auto callExprs = index.Nodes(nodes::CallExpression);
    CHECK(callExprs.size() == 2);

    auto varDecls = index.Nodes(nodes::VariableDeclaration);
    CHECK(varDecls.size() == 2);

    // Verify symbol lookup
    TSSymbol funcSym = index.SymbolForName(nodes::FuncDeclaration);
    CHECK(funcSym != 0);
    auto funcDeclsBySym = index.Nodes(funcSym);
    CHECK(funcDeclsBySym.size() == 2);

    // Verify nonexistent symbol
    CHECK(index.Nodes("nonexistent_node_type").empty());
    CHECK(index.SymbolForName("nonexistent_node_type") == 0);

    // Verify AllNodes contains all nodes
    auto all = index.AllNodes();
    CHECK(all.size() == index.NodeCount());
}
