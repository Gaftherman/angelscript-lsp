#include "analysis/NodeIndex.h"
#include "document/Document.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/TestUtils.h"
#include "parser/ASTUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include <algorithm>
#include <doctest/doctest.h>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("AstStructuralTraversal - Null node safety")
{
    TSNode nullNode{};
    CHECK(ts_node_is_null(parser::GetRootNode(nullNode)));
    CHECK(ts_node_is_null(parser::GetParent(nullNode)));
}

TEST_CASE("AstStructuralTraversal - GetRootNode O(1) parity with upward parent climb")
{
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Method");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("myVar");

    const std::string code = std::string("class ") + clsName + "\n{\n" + "    void " + fnName + "()\n    {\n" +
                             "        int " + varName + " = 42;\n" + "    }\n}\n";

    auto doc = angel_lsp::test::CreateTestDocument("file:///parity_ast_traversal.as", code);
    REQUIRE(doc->GetTree() != nullptr);

    TSNode expectedRoot = ts_tree_root_node(doc->GetTree());
    REQUIRE(!ts_node_is_null(expectedRoot));

    NodeIndex index(expectedRoot);
    auto varDeclarators = index.Nodes(nodes::VariableDeclarator);
    REQUIRE(!varDeclarators.empty());

    TSNode leaf = varDeclarators.front();
    REQUIRE(!ts_node_is_null(leaf));

    // O(1) GetRootNode
    TSNode fastRoot = parser::GetRootNode(leaf);
    CHECK(!ts_node_is_null(fastRoot));
    CHECK(ts_node_eq(fastRoot, expectedRoot));

    // Iterative climb
    TSNode climbRoot = leaf;
    while (!ts_node_is_null(parser::GetParent(climbRoot)))
    {
        climbRoot = parser::GetParent(climbRoot);
    }

    CHECK(ts_node_eq(fastRoot, climbRoot));
}

TEST_CASE("AstStructuralTraversal - Template brackets get templatePunctuation token type")
{
    const auto& legend = features::GetSemanticTokensLegend();
    const auto it = std::find(legend.tokenTypes.begin(), legend.tokenTypes.end(), "templatePunctuation");
    REQUIRE(it != legend.tokenTypes.end());
    const uint32_t templatePunctuation = static_cast<uint32_t>(std::distance(legend.tokenTypes.begin(), it));

    const auto opIt = std::find(legend.tokenTypes.begin(), legend.tokenTypes.end(), "operator");
    REQUIRE(opIt != legend.tokenTypes.end());
    const uint32_t operatorType = static_cast<uint32_t>(std::distance(legend.tokenTypes.begin(), opIt));

    const std::string varArr = angel_lsp::test::GenerateRandomSymbolName("arr");
    const std::string varA = angel_lsp::test::GenerateRandomSymbolName("a");
    const std::string varB = angel_lsp::test::GenerateRandomSymbolName("b");

    const std::string code = std::string("void test()\n{\n") + "    array<int> " + varArr + ";\n" +
                             "    bool cmp = " + varA + " < " + varB + ";\n" + "}\n";

    AngelScriptParser parser;
    document::TreePtr tree = document::MakeTreePtr(parser.Parse(code));
    REQUIRE(tree != nullptr);

    SymbolTable table;
    features::SemanticTokensRequest req{"file:///template_punct.as", code, tree.get(), table};
    const auto tokens = features::GetSemanticTokens(req);

    REQUIRE(tokens.data.size() % 5 == 0);

    bool hasTemplatePunctuation = false;
    bool hasOperator = false;
    for (size_t i = 0; i + 4 < tokens.data.size(); i += 5)
    {
        const uint32_t type = tokens.data[i + 3];
        if (type == templatePunctuation)
        {
            hasTemplatePunctuation = true;
        }
        else if (type == operatorType)
        {
            hasOperator = true;
        }
    }
    CHECK(hasTemplatePunctuation);
    CHECK(hasOperator);
}
