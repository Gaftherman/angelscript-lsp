#include <doctest/doctest.h>

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <array>
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

TEST_CASE("SemanticTokensHandler - An unchanged stream produces no edits")
{
    const std::vector<lsp::uint> tokens{ 0, 0, 3, 15, 0, 0, 4, 5, 8, 0 };
    CHECK(ComputeSemanticTokensDelta(tokens, tokens).empty());
}

TEST_CASE("SemanticTokensHandler - A delta splices only the run that changed")
{
    const std::vector<lsp::uint> previous{ 0, 0, 3, 15, 0, /**/ 1, 0, 4, 12, 0, /**/ 1, 0, 5, 8, 0 };
    const std::vector<lsp::uint> current{ 0, 0, 3, 15, 0, /**/ 1, 0, 7, 12, 0, /**/ 1, 0, 5, 8, 0 };

    const auto edits = ComputeSemanticTokensDelta(previous, current);
    REQUIRE(edits.size() == 1);

    // Only the one changed integer is resent: the untouched runs on either side are what the
    // prefix/suffix scan is for.
    CHECK(edits[0].start == 7);
    CHECK(edits[0].deleteCount == 1);
    REQUIRE(edits[0].data.has_value());
    REQUIRE(edits[0].data->size() == 1);
    CHECK((*edits[0].data)[0] == 7);
}

TEST_CASE("SemanticTokensHandler - A delta describes an appended token")
{
    const std::vector<lsp::uint> previous{ 0, 0, 3, 15, 0 };
    const std::vector<lsp::uint> current{ 0, 0, 3, 15, 0, 1, 0, 4, 12, 0 };

    const auto edits = ComputeSemanticTokensDelta(previous, current);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].start == 5);
    CHECK(edits[0].deleteCount == 0);
    REQUIRE(edits[0].data.has_value());
    CHECK(edits[0].data->size() == 5);
}

TEST_CASE("SemanticTokensHandler - A delta describes a removed token")
{
    const std::vector<lsp::uint> previous{ 0, 0, 3, 15, 0, 1, 0, 4, 12, 0 };
    const std::vector<lsp::uint> current{ 0, 0, 3, 15, 0 };

    const auto edits = ComputeSemanticTokensDelta(previous, current);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].start == 5);
    CHECK(edits[0].deleteCount == 5);
    CHECK_FALSE(edits[0].data.has_value());
}

TEST_CASE("SemanticTokensHandler - A delta against an empty stream sends everything")
{
    const std::vector<lsp::uint> current{ 0, 0, 3, 15, 0 };

    const auto edits = ComputeSemanticTokensDelta({}, current);
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].start == 0);
    CHECK(edits[0].deleteCount == 0);
    REQUIRE(edits[0].data.has_value());
    CHECK(*edits[0].data == lsp::Array<lsp::uint>(current.begin(), current.end()));
}

TEST_CASE("SemanticTokensHandler - Applying the edits reproduces the new stream")
{
    const std::vector<lsp::uint> previous{ 0, 0, 3, 15, 0, 1, 0, 4, 12, 0, 1, 0, 5, 8, 0 };
    const std::vector<lsp::uint> current{ 0, 0, 3, 15, 0, 1, 0, 9, 12, 0, 2, 0, 5, 8, 0, 1, 0, 2, 8, 0 };

    auto applied = previous;
    for (const auto &edit : ComputeSemanticTokensDelta(previous, current))
    {
        const auto first = applied.begin() + static_cast<std::ptrdiff_t>(edit.start);
        applied.erase(first, first + static_cast<std::ptrdiff_t>(edit.deleteCount));
        if (edit.data.has_value())
        {
            applied.insert(applied.begin() + static_cast<std::ptrdiff_t>(edit.start),
                           edit.data->begin(), edit.data->end());
        }
    }

    CHECK(applied == current);
}

// =====================================================================================
// Template brackets are not shift operators.
//
// The TextMate grammar cannot tell them apart - its operator rule matches `>>` unconditionally, as
// one two-character token - so `array<array<int>>` closed with something scoped
// `keyword.operator.angelscript`. Only the parse tree knows better, and this pass has one.
//
// It used to `continue` here, emitting nothing at all, which left the client with no semantic token
// to override the TextMate scope with. The fix is to emit one; `templatePunctuation` is a custom
// type the extension contributes (client/package.json) precisely so it can be themed apart from
// the arithmetic operators.
// =====================================================================================

TEST_CASE("SemanticTokensHandler - Template brackets get their own token type")
{
    const auto &legend = GetSemanticTokensLegend();
    const auto it = std::find(legend.tokenTypes.begin(), legend.tokenTypes.end(), "templatePunctuation");
    REQUIRE(it != legend.tokenTypes.end());
    const uint32_t templatePunctuation = static_cast<uint32_t>(std::distance(legend.tokenTypes.begin(), it));

    const auto operatorIt = std::find(legend.tokenTypes.begin(), legend.tokenTypes.end(), "operator");
    REQUIRE(operatorIt != legend.tokenTypes.end());
    const uint32_t operatorType = static_cast<uint32_t>(std::distance(legend.tokenTypes.begin(), operatorIt));

    const std::string code =
        "void main()\n"                          // 0
        "{\n"                                    // 1
        "    array<array<int>> grid;\n"          // 2
        "    int shifted = 1 << 2;\n"            // 3
        "}\n";                                   // 4

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest req{ "file:///template.as", code, tree, table };
    const auto tokens = GetSemanticTokens(req);
    ts_tree_delete(tree);

    REQUIRE(tokens.data.size() % 5 == 0);

    // Rebuild absolute lines from the delta encoding, so each token can be attributed to a line.
    uint32_t line = 0;
    size_t templateTokensOnDeclaration = 0;
    size_t operatorTokensOnShift = 0;
    for (size_t i = 0; i + 4 < tokens.data.size(); i += 5)
    {
        line += tokens.data[i];
        const uint32_t type = tokens.data[i + 3];
        if (line == 2 && type == templatePunctuation)
        {
            ++templateTokensOnDeclaration;
        }
        if (line == 3 && type == operatorType)
        {
            ++operatorTokensOnShift;
        }
    }

    // Four brackets in `array<array<int>>`, each its own token - the pair of closers included, which
    // is the case TextMate reads as a single `>>`.
    CHECK(templateTokensOnDeclaration == 4);

    // And the genuine shift on the next line is still an operator.
    CHECK(operatorTokensOnShift > 0);
}

// =====================================================================================
// Dead preprocessor blocks are painted as comments.
//
// The server already stays silent inside an excluded `#if`, which is half the truth: the compiler
// never sees that code. The other half is that it looked exactly like live code, so a reader had no
// way to tell the difference and would wonder why nothing there was ever reported.
// =====================================================================================

namespace
{
    /** @brief The (line, startChar, length, tokenType) of every token, decoded from the payload. */
    std::vector<std::array<uint32_t, 4>> DecodeTokens(const std::vector<lsp::uint> &data)
    {
        std::vector<std::array<uint32_t, 4>> out;
        uint32_t line = 0;
        uint32_t character = 0;

        for (size_t i = 0; i + 4 < data.size(); i += 5)
        {
            const uint32_t deltaLine = data[i];
            const uint32_t deltaStart = data[i + 1];

            line += deltaLine;
            character = (deltaLine == 0) ? character + deltaStart : deltaStart;
            out.push_back({ line, character, data[i + 2], data[i + 3] });
        }
        return out;
    }

    /** @brief Index of "comment" in the legend, found by name so a reordering cannot silently pass. */
    uint32_t CommentTokenType()
    {
        const auto &types = GetSemanticTokensLegend().tokenTypes;
        for (uint32_t i = 0; i < types.size(); ++i)
        {
            if (types[i] == "comment")
                return i;
        }
        return UINT32_MAX;
    }
}

TEST_CASE("SemanticTokensHandler - An excluded #if block is emitted as comment tokens")
{
    const std::string code =
        "int live = 1;\n"        // 0
        "#if NOT_DEFINED\n"      // 1
        "int dead = 2;\n"        // 2
        "#endif\n"               // 3
        "int alsoLive = 3;\n";   // 4

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;
    SemanticTokensRequest req{ "file:///dead.as", code, tree, table };
    req.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(code);

    // Precondition: the block really is excluded, or the rest of this proves nothing.
    REQUIRE_FALSE(req.excludedLineRanges.empty());

    const auto tokens = DecodeTokens(GetSemanticTokens(req).data);
    const uint32_t comment = CommentTokenType();
    REQUIRE(comment != UINT32_MAX);

    const auto onLine = [&tokens](uint32_t line)
    {
        std::vector<std::array<uint32_t, 4>> found;
        for (const auto &t : tokens)
        {
            if (t[0] == line)
                found.push_back(t);
        }
        return found;
    };

    // Every excluded line - the directives included, because CScriptBuilder blanks those too - is
    // one comment token spanning the whole line.
    for (const uint32_t dead : { 1u, 2u, 3u })
    {
        CAPTURE(dead);
        const auto found = onLine(dead);
        REQUIRE(found.size() == 1);
        CHECK(found[0][1] == 0);                  // starts at the beginning of the line
        CHECK(found[0][3] == comment);
    }

    // And the live lines are untouched: whatever they had, none of it became a comment.
    for (const uint32_t alive : { 0u, 4u })
    {
        CAPTURE(alive);
        const auto found = onLine(alive);
        CHECK_FALSE(found.empty());
        for (const auto &t : found)
            CHECK(t[3] != comment);
    }

    ts_tree_delete(tree);
}

TEST_CASE("SemanticTokensHandler - With nothing excluded the payload is unchanged")
{
    // The guard on the feature: a document with no dead block must not gain a single token.
    const std::string code = "int a = 1;\nvoid main() { }\n";

    AngelScriptParser parser;
    TSTree *tree = parser.Parse(code);
    REQUIRE(tree != nullptr);

    SymbolTable table;

    SemanticTokensRequest without{ "file:///plain.as", code, tree, table };
    SemanticTokensRequest with{ "file:///plain.as", code, tree, table };
    with.excludedLineRanges = angel_lsp::utils::FindExcludedLineRanges(code);

    CHECK(with.excludedLineRanges.empty());
    CHECK(GetSemanticTokens(without).data == GetSemanticTokens(with).data);

    ts_tree_delete(tree);
}
