#include "analysis/NodeIndex.h"
#include "helpers/TestUtils.h"
#include "parser/GrammarNames.h"
#include <doctest/doctest.h>

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
    CHECK(index.ErrorRanges().empty());
}

TEST_CASE("NodeIndex - Error range indexing and containment invariant")
{
    const std::string validFn = angel_lsp::test::GenerateRandomSymbolName();
    const std::string brokenFn = angel_lsp::test::GenerateRandomSymbolName();
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName();

    // Valid code has zero error ranges
    const std::string cleanCode = "void " + validFn + "() { int " + varName + " = 42; }\n";
    auto cleanDoc = angel_lsp::test::CreateTestDocument("file:///clean_node_index.as", cleanCode);
    REQUIRE(cleanDoc->GetTree() != nullptr);
    NodeIndex cleanIndex(ts_tree_root_node(cleanDoc->GetTree()));
    CHECK(cleanIndex.ErrorRanges().empty());
    CHECK_FALSE(cleanIndex.IsInsideError(0));
    CHECK_FALSE(cleanIndex.IsInsideError(10));
    CHECK_FALSE(cleanIndex.NodeHasSyntaxError(cleanIndex.Root()));

    // Broken code with syntax error produces error ranges
    const std::string brokenCode = "void " + brokenFn +
                                   "() {\n"
                                   "    int " +
                                   varName +
                                   " = ;\n" // missing expression syntax error
                                   "}\n";
    auto brokenDoc = angel_lsp::test::CreateTestDocument("file:///broken_node_index.as", brokenCode);
    REQUIRE(brokenDoc->GetTree() != nullptr);
    NodeIndex brokenIndex(ts_tree_root_node(brokenDoc->GetTree()));
    CHECK(!brokenIndex.ErrorRanges().empty());

    // The semicolon line has a syntax error
    const auto ranges = brokenIndex.ErrorRanges();
    CHECK(!ranges.empty());
    const uint32_t errStart = ranges.front().startByte;
    CHECK(brokenIndex.IsInsideError(errStart));
    CHECK_FALSE(brokenIndex.IsInsideError(0)); // Start of function is clean
}
