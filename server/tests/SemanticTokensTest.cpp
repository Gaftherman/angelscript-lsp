#include <doctest/doctest.h>

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("SemanticTokensHandler - Legend is populated")
{
    const auto &legend = GetSemanticTokensLegend();
    CHECK(!legend.tokenTypes.empty());
    CHECK(!legend.tokenModifiers.empty());

    // Check key standard token types
    bool hasFunction = false;
    bool hasVariable = false;
    bool hasKeyword = false;
    for (const auto &tt : legend.tokenTypes)
    {
        if (tt == "function") hasFunction = true;
        if (tt == "variable") hasVariable = true;
        if (tt == "keyword") hasKeyword = true;
    }
    CHECK(hasFunction);
    CHECK(hasVariable);
    CHECK(hasKeyword);
}

TEST_CASE("SemanticTokensHandler - Delta Encoding for Simple Script")
{
    std::string code = 
        "// Comment\n"
        "int x = 42;\n"
        "void main() {\n"
        "    Print(x);\n"
        "}\n";

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest req{ "file:///test.as", code, tree, table };
    auto tokens = GetSemanticTokens(req);

    // The data array contains 5-tuples: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers]
    REQUIRE(tokens.data.size() % 5 == 0);
    REQUIRE(tokens.data.size() > 0);

    // First token is comment on line 0
    CHECK(tokens.data[0] == 0); // line 0
    CHECK(tokens.data[1] == 0); // col 0
    CHECK(tokens.data[2] == 10); // length of "// Comment"

    ts_tree_delete(tree);
}

TEST_CASE("SemanticTokensHandler - Empty Code Returns Empty Tokens")
{
    AngelScriptParser parser;
    TSTree *tree = parser.Parse("");
    SymbolTable table;
    SemanticTokensRequest req{ "file:///test.as", "", tree, table };
    auto tokens = GetSemanticTokens(req);
    CHECK(tokens.data.empty());
    if (tree) ts_tree_delete(tree);
}
