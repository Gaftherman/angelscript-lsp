#include <doctest/doctest.h>

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("SemanticTokensHandler - Primitive Types Map to Type_Keyword")
{
    std::string code = "int a = 1;\nfloat b = 2.0f;\nbool c = true;\nauto d = 4;\n";

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest req{ "file:///test.as", code, tree, table };
    auto tokens = GetSemanticTokens(req);

    REQUIRE(tokens.data.size() % 5 == 0);

    struct DecodedToken
    {
        uint32_t line;
        uint32_t startCol;
        uint32_t length;
        uint32_t tokenType;
        uint32_t tokenMod;
    };

    std::vector<DecodedToken> decoded;
    uint32_t curLine = 0;
    uint32_t curCol = 0;
    for (size_t i = 0; i < tokens.data.size(); i += 5)
    {
        uint32_t deltaLine = tokens.data[i];
        uint32_t deltaCol = tokens.data[i + 1];
        uint32_t len = tokens.data[i + 2];
        uint32_t type = tokens.data[i + 3];
        uint32_t mod = tokens.data[i + 4];

        if (deltaLine > 0)
        {
            curLine += deltaLine;
            curCol = deltaCol;
        }
        else
        {
            curCol += deltaCol;
        }

        decoded.push_back({ curLine, curCol, len, type, mod });
    }

    // Check that int (line 0, col 0, len 3) is tokenType 15 (Type_Keyword)
    auto itInt = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedToken &t) { return t.line == 0 && t.startCol == 0 && t.length == 3; });
    REQUIRE(itInt != decoded.end());
    CHECK(itInt->tokenType == 15);
    CHECK(itInt->tokenMod == 0);

    // Check that float (line 1, col 0, len 5) is tokenType 15
    auto itFloat = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedToken &t) { return t.line == 1 && t.startCol == 0 && t.length == 5; });
    REQUIRE(itFloat != decoded.end());
    CHECK(itFloat->tokenType == 15);
    CHECK(itFloat->tokenMod == 0);

    // Check that bool (line 2, col 0, len 4) is tokenType 15
    auto itBool = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedToken &t) { return t.line == 2 && t.startCol == 0 && t.length == 4; });
    REQUIRE(itBool != decoded.end());
    CHECK(itBool->tokenType == 15);
    CHECK(itBool->tokenMod == 0);

    // Check that auto (line 3, col 0, len 4) is tokenType 15
    auto itAuto = std::find_if(decoded.begin(), decoded.end(),
        [](const DecodedToken &t) { return t.line == 3 && t.startCol == 0 && t.length == 4; });
    REQUIRE(itAuto != decoded.end());
    CHECK(itAuto->tokenType == 15);
    CHECK(itAuto->tokenMod == 0);

    ts_tree_delete(tree);
}

namespace
{
    /** @brief Decodes a delta-encoded token stream back into absolute (line, startChar) pairs. */
    std::vector<std::pair<uint32_t, uint32_t>> DecodeTokenPositions(const std::vector<lsp::uint> &data)
    {
        std::vector<std::pair<uint32_t, uint32_t>> positions;
        uint32_t line = 0;
        uint32_t character = 0;

        for (size_t i = 0; i + 4 < data.size(); i += 5)
        {
            const uint32_t deltaLine = data[i];
            const uint32_t deltaStart = data[i + 1];

            line += deltaLine;
            character = (deltaLine == 0) ? character + deltaStart : deltaStart;
            positions.emplace_back(line, character);
        }
        return positions;
    }

    const std::string k_rangeSource =
        "int alpha = 1;\n"
        "int beta = 2;\n"
        "int gamma = 3;\n"
        "int delta = 4;\n";
}

TEST_CASE("SemanticTokensHandler - A ranged request returns only the tokens it overlaps")
{
    AngelScriptParser parser;
    TSTree *tree = parser.Parse(k_rangeSource);
    REQUIRE(tree != nullptr);

    SymbolTable table;

    SemanticTokensRequest fullRequest{ "file:///range.as", k_rangeSource, tree, table };
    const auto fullPositions = DecodeTokenPositions(GetSemanticTokens(fullRequest).data);
    REQUIRE(!fullPositions.empty());

    SemanticTokensRequest rangedRequest{ "file:///range.as", k_rangeSource, tree, table };
    rangedRequest.range = lsp::Range{ { 1, 0 }, { 2, 0 } };
    const auto rangedPositions = DecodeTokenPositions(GetSemanticTokens(rangedRequest).data);

    REQUIRE(!rangedPositions.empty());
    for (const auto &[line, character] : rangedPositions)
    {
        CHECK(line == 1);
    }

    // Every token the range kept has to be one the full pass also produced, at the same place:
    // narrowing must not change how a token is classified or where it starts.
    for (const auto &position : rangedPositions)
    {
        CHECK(std::find(fullPositions.begin(), fullPositions.end(), position) != fullPositions.end());
    }

    CHECK(rangedPositions.size() < fullPositions.size());

    ts_tree_delete(tree);
}

TEST_CASE("SemanticTokensHandler - The first token of a range is encoded against the origin")
{
    AngelScriptParser parser;
    TSTree *tree = parser.Parse(k_rangeSource);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest request{ "file:///range.as", k_rangeSource, tree, table };
    request.range = lsp::Range{ { 2, 0 }, { 3, 0 } };

    const auto tokens = GetSemanticTokens(request);
    REQUIRE(tokens.data.size() >= 5);

    // The stream is delta-encoded against its own predecessor, so a slice whose first entry still
    // carried the delta from the token before it would place every token two lines too far down.
    CHECK(tokens.data[0] == 2);
    CHECK(tokens.data[1] == 0);

    ts_tree_delete(tree);
}

TEST_CASE("SemanticTokensHandler - An absent range is identical to a full request")
{
    AngelScriptParser parser;
    TSTree *tree = parser.Parse(k_rangeSource);
    REQUIRE(tree != nullptr);

    SymbolTable table;

    SemanticTokensRequest withoutRange{ "file:///range.as", k_rangeSource, tree, table };
    SemanticTokensRequest wholeDocument{ "file:///range.as", k_rangeSource, tree, table };
    wholeDocument.range = lsp::Range{ { 0, 0 }, { 100, 0 } };

    CHECK(GetSemanticTokens(withoutRange).data == GetSemanticTokens(wholeDocument).data);

    ts_tree_delete(tree);
}

TEST_CASE("SemanticTokensHandler - A range covering no tokens returns an empty stream")
{
    const std::string code =
        "int alpha = 1;\n"
        "\n"
        "\n"
        "int beta = 2;\n";

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest request{ "file:///empty-range.as", code, tree, table };
    request.range = lsp::Range{ { 1, 0 }, { 2, 0 } };

    CHECK(GetSemanticTokens(request).data.empty());

    ts_tree_delete(tree);
}
