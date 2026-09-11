#include <doctest/doctest.h>

#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include "parser/Keywords.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <lsp/json/json.h>
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

// A primitive used to be reported as a keyword, and themes paint a keyword the colour of `if`.
// So `float` read as control flow while every other type on the line read as a type - reported from
// use, with the editor's token inspector showing `semantic token type keyword` over a textmate scope
// of storage.type.built-in.primitive.angelscript. The textmate grammar had the better answer and the
// semantic token was overriding it.
TEST_CASE("SemanticTokensHandler - A primitive is reported as a type from the default library")
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

    // Type_Type, carrying Mod_DefaultLibrary so a theme can still tell `float` from a class the
    // user wrote - the distinction "keyword" was reaching for, made without claiming it is one.
    constexpr uint32_t k_type = 1;
    constexpr uint32_t k_defaultLibrary = 1u << 9;

    // The two constants are captured explicitly. doctest's CHECK takes its operands by reference to
    // build the failure message, which odr-uses them - MSVC lets that through on a constexpr local,
    // GCC does not, and the difference only appeared in the Linux container: "'k_type' is not
    // captured".
    const auto require = [&decoded, k_type, k_defaultLibrary](uint32_t line, uint32_t length, const char *what)
    {
        auto it = std::find_if(decoded.begin(), decoded.end(),
            [line, length](const DecodedToken &t)
            { return t.line == line && t.startCol == 0 && t.length == length; });
        INFO("primitive: " << what);
        REQUIRE(it != decoded.end());
        CHECK(it->tokenType == k_type);
        CHECK(it->tokenMod == k_defaultLibrary);
    };

    require(0, 3, "int");
    require(1, 5, "float");
    require(2, 4, "bool");
    require(3, 4, "auto");

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

    // The edit must be strictly aligned to 5-tuples (whole tokens) to prevent modulo-5 desync.
    CHECK(edits[0].start == 5);
    CHECK(edits[0].deleteCount == 5);
    REQUIRE(edits[0].data.has_value());
    REQUIRE(edits[0].data->size() == 5);
    CHECK((*edits[0].data)[2] == 7);
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

    // CommentTokenType() lived here, and went unused when the test below stopped requiring one
    // comment token per dead line - see its own comment for why that instrument was wrong. GCC
    // said so (-Wunused-function) and MSVC did not, which is the only reason it survived this long.
}

TEST_CASE("SemanticTokensHandler - An excluded #if block is left to the decoration")
{
    // This used to require one full-line comment token per dead line. That was the wrong
    // instrument twice over. It could not reach the editor's bracket-pair colouring, which paints
    // `(`, `{` and `[` from its own feature and consults neither TextMate nor semantic scopes - so
    // dead code kept rainbow brackets whatever was emitted for it, which is how the defect was
    // spotted. And where it did apply, it threw away the difference between a comment and code that
    // simply is not compiled.
    //
    // The dimming is a decoration now: the server sends angelscript/inactiveRegions and the client
    // dims the whole region, brackets included, which is what the C++ extension does. What this
    // handler owes is silence - no semantic token on a dead line, so the syntax colours underneath
    // show through and the decoration dims them.
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

    // Every excluded line - the directives included, because CScriptBuilder blanks those too -
    // carries no semantic token at all.
    for (const uint32_t dead : { 1u, 2u, 3u })
    {
        CAPTURE(dead);
        CHECK(onLine(dead).empty());
    }

    // And the live lines still have theirs.
    for (const uint32_t alive : { 0u, 4u })
    {
        CAPTURE(alive);
        CHECK_FALSE(onLine(alive).empty());
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

// =====================================================================================
// What colour each name comes out.
//
// The existing tests here check that the legend is populated, that the delta encoding is
// well-formed, and that *some* token of a few kinds exists. None of them checks that a particular
// name gets a particular type - which is the whole of what a user sees. A class coloured as a
// variable, or a parameter coloured as a local, is invisible to every assertion in this file.
//
// The scenarios live in tests/fixtures/token_scenarios.json and name a position, the text that must
// be there, and the type it must carry. The text is checked too, so an expectation whose position
// drifted fails as a bad expectation rather than as a server defect.
// =====================================================================================

namespace
{
    struct TokenExpectation
    {
        uint32_t line = 0;
        uint32_t character = 0;
        std::string text;
        std::string type;

        /**
         * @brief Why this one is still wrong, when it is.
         *
         * Empty for an expectation the server meets. A non-empty reason is a colour that is
         * measurably wrong today and understood - the same bookkeeping the parity audit keeps for
         * the compiler, and for the same reason: a gap nobody wrote down is a gap nobody fixes, and
         * one that fails the build is a gap somebody deletes.
         */
        std::string gap;
    };

    struct TokenScenario
    {
        std::string name;
        std::string why;
        std::string source;
        std::vector<TokenExpectation> expect;
    };

    std::vector<TokenScenario> LoadTokenScenarios(const std::string &fileName)
    {
        const std::filesystem::path path = std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / fileName;

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("scenarios");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<TokenScenario> scenarios;
        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            TokenScenario scenario;
            scenario.name = fields.find("name")->string();
            scenario.why = fields.find("why")->string();
            scenario.source = fields.find("source")->string();

            for (const auto &item : fields.find("expect")->array())
            {
                const lsp::json::Object &expectation = item.object();
                TokenExpectation expected;
                expected.line = static_cast<uint32_t>(expectation.find("line")->number());
                expected.character = static_cast<uint32_t>(expectation.find("character")->number());
                expected.text = expectation.find("text")->string();
                expected.type = expectation.find("type")->string();

                if (const auto *gap = expectation.find("gap"); gap && gap->isString())
                    expected.gap = gap->string();

                scenario.expect.push_back(std::move(expected));
            }

            scenarios.push_back(std::move(scenario));
        }

        return scenarios;
    }

    /** @brief One token, with its position resolved out of the protocol's delta encoding. */
    struct AbsoluteToken
    {
        uint32_t line = 0;
        uint32_t character = 0;
        uint32_t length = 0;
        std::string type;
    };

    std::vector<AbsoluteToken> DecodeAbsoluteTokens(const std::vector<unsigned> &data)
    {
        const auto &legend = GetSemanticTokensLegend();

        std::vector<AbsoluteToken> tokens;
        uint32_t line = 0;
        uint32_t character = 0;

        for (size_t i = 0; i + 4 < data.size(); i += 5)
        {
            const uint32_t deltaLine = data[i];
            const uint32_t deltaStart = data[i + 1];

            line += deltaLine;
            character = deltaLine == 0 ? character + deltaStart : deltaStart;

            AbsoluteToken token;
            token.line = line;
            token.character = character;
            token.length = data[i + 2];
            token.type = data[i + 3] < legend.tokenTypes.size() ? legend.tokenTypes[data[i + 3]]
                                                                : std::string("<out of legend>");
            tokens.push_back(token);
        }

        return tokens;
    }

    /** @brief The source text at a position, so a drifted expectation is reported as its own fault. */
    std::string TextAt(const std::string &source, uint32_t line, uint32_t character, size_t length)
    {
        size_t at = 0;
        for (uint32_t skipped = 0; skipped < line; ++skipped)
        {
            at = source.find('\n', at);
            if (at == std::string::npos)
                return {};
            ++at;
        }

        at += character;
        if (at >= source.size())
            return {};

        return source.substr(at, length);
    }
}

TEST_CASE("SemanticTokensHandler - Every name carries the type its colour comes from")
{
    // Two corpora, run as one. The first covers the everyday shapes - a class, a method, a
    // parameter, a literal. The second reaches the grammar that one does not: a `#if` block the
    // preprocessor drops, funcdefs and typedefs, `array<array<int>>`, an anonymous function, a
    // cast. They are separate files because they were generated separately and measured
    // separately; they are one loop because every assertion about a colour is the same assertion.
    std::vector<TokenScenario> scenarios = LoadTokenScenarios("token_scenarios.json");
    const std::vector<TokenScenario> grammar = LoadTokenScenarios("token_grammar_scenarios.json");
    scenarios.insert(scenarios.end(), grammar.begin(), grammar.end());

    REQUIRE_FALSE(scenarios.empty());

    size_t met = 0;
    size_t gaps = 0;

    for (const TokenScenario &scenario : scenarios)
    {
        CAPTURE(scenario.name);
        INFO(scenario.why);

        AngelScriptParser parser;
        TSTree *tree = parser.Parse(scenario.source);
        REQUIRE(tree != nullptr);

        // With an empty table a class is just an identifier, so the symbols have to be collected
        // first - which is what the server does before asking for tokens.
        SymbolCollector collector{ nullptr };
        SymbolTable table;
        collector.CollectSymbols("file:///tokens.as", scenario.source, parser, table);

        // And the scope tree, which is what tells a *use* of a parameter apart from a use of a
        // local. The server passes it; leaving it out here made three parameter uses look like a
        // server defect when the omission was this test's.
        LocalScopeCollector scopeCollector{ nullptr };
        std::shared_ptr<const Scope> scopeRoot = scopeCollector.CollectScopes(scenario.source, parser);

        SemanticTokensRequest request{ "file:///tokens.as", scenario.source, tree, table, scopeRoot };
        const auto tokens = DecodeAbsoluteTokens(GetSemanticTokens(request).data);

        for (const TokenExpectation &expected : scenario.expect)
        {
            CAPTURE(expected.text);
            CAPTURE(expected.line);
            CAPTURE(expected.character);

            // The expectation has to point at what it says it does, or a failure below would blame
            // the server for a position someone counted wrong.
            const std::string actualText = TextAt(scenario.source, expected.line, expected.character,
                                                  expected.text.size());
            CHECK(actualText == expected.text);

            const auto found = std::find_if(tokens.begin(), tokens.end(),
                                            [&expected](const AbsoluteToken &token) {
                                                return token.line == expected.line &&
                                                       token.character == expected.character;
                                            });

            const bool matched = found != tokens.end() && found->type == expected.type;

            if (!expected.gap.empty())
            {
                // A gap that has been fixed has to stop being called one, or this file starts
                // excusing work that is already done.
                CHECK_MESSAGE(!matched,
                              "'" << expected.text << "' in " << scenario.name
                                  << " is marked as a known gap but now carries " << expected.type
                                  << " - remove the gap from token_scenarios.json");
                ++gaps;
                continue;
            }

            if (found == tokens.end())
            {
                FAIL_CHECK("no token starts at " << expected.line << ":" << expected.character
                                                 << " for '" << expected.text << "'");
                continue;
            }

            INFO("expected " << expected.type << ", got " << found->type);
            CHECK(found->type == expected.type);
            ++met;
        }

        ts_tree_delete(tree);
    }

    MESSAGE("semantic tokens: " << met << " expectations met, " << gaps << " known gaps");
}


// =====================================================================================
// The colouring sweep.
//
// Every case above asks about one construct someone thought to check. This asks the opposite
// question - is there anything we forgot - and it is the question that found the reported bugs:
// `and` and `not` came back as operators, so a theme painted them the colour of `;`, and `float`
// came back as a keyword, so it was painted the colour of `if` while every other type on the line
// was painted as a type.
//
// Two halves, and they only mean something together. The sweep says every word of the language is
// classified as *something*, which catches a construct nobody wired up - `foreach` was missing from
// one of the three keyword lists this project used to keep, and nothing noticed. The spot checks
// below say it is classified as the *right* thing, which the sweep cannot see: an operator and a
// keyword are both "classified".
// =====================================================================================

namespace
{
    struct SweptToken
    {
        uint32_t line;
        uint32_t startCol;
        uint32_t length;
        uint32_t type;
        uint32_t mod;
    };

    /** @brief Semantic tokens for a document, decoded back to absolute positions. */
    std::vector<SweptToken> SweepTokens(const std::string &code)
    {
        AngelScriptParser parser;
        TSTree *tree = parser.Parse(code);
        REQUIRE(tree != nullptr);

        SymbolTable table;
        SemanticTokensRequest request{ "file:///sweep.as", code, tree, table };
        const auto tokens = GetSemanticTokens(request);
        ts_tree_delete(tree);

        std::vector<SweptToken> swept;
        uint32_t line = 0;
        uint32_t column = 0;

        for (size_t i = 0; i + 4 < tokens.data.size(); i += 5)
        {
            const uint32_t deltaLine = tokens.data[i];
            const uint32_t deltaStart = tokens.data[i + 1];

            if (deltaLine > 0)
            {
                line += deltaLine;
                column = deltaStart;
            }
            else
            {
                column += deltaStart;
            }

            swept.push_back(SweptToken{ line, column, tokens.data[i + 2], tokens.data[i + 3], tokens.data[i + 4] });
        }

        return swept;
    }

    /** @brief The token covering a position, or nullptr. Containment, not equality: `!is` starts a
     *         character before the word `is` that the scan below finds. */
    const SweptToken *TokenCovering(const std::vector<SweptToken> &swept, uint32_t line, uint32_t column)
    {
        for (const auto &token : swept)
        {
            if (token.line == line && column >= token.startCol && column < token.startCol + token.length)
                return &token;
        }
        return nullptr;
    }

    /** @brief The token that starts exactly here, or nullptr. */
    const SweptToken *TokenAt(const std::vector<SweptToken> &swept, uint32_t line, uint32_t column)
    {
        for (const auto &token : swept)
        {
            if (token.line == line && token.startCol == column)
                return &token;
        }
        return nullptr;
    }

    /** @brief The column of a needle on a line, so no test has to count characters by hand. */
    uint32_t ColumnOf(const std::string &code, uint32_t line, const std::string &needle)
    {
        size_t start = 0;
        for (uint32_t current = 0; current < line; ++current)
        {
            start = code.find('\n', start);
            REQUIRE(start != std::string::npos);
            ++start;
        }

        const size_t lineEnd = code.find('\n', start);
        const std::string text = code.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start);

        const size_t at = text.find(needle);
        REQUIRE(at != std::string::npos);
        return static_cast<uint32_t>(at);
    }

    struct WordSite
    {
        uint32_t line;
        uint32_t column;
        std::string word;
    };

    /** @brief Every place a reserved word appears as a whole word, wherever it appears. */
    std::vector<WordSite> ReservedWordSites(const std::string &code)
    {
        const auto isWordChar = [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };

        std::vector<WordSite> sites;
        uint32_t line = 0;
        uint32_t column = 0;

        for (size_t i = 0; i < code.size();)
        {
            if (code[i] == '\n')
            {
                ++line;
                column = 0;
                ++i;
                continue;
            }

            if (!isWordChar(code[i]) || (i > 0 && isWordChar(code[i - 1])))
            {
                ++column;
                ++i;
                continue;
            }

            size_t end = i;
            while (end < code.size() && isWordChar(code[end]))
                ++end;

            const std::string word = code.substr(i, end - i);
            if (angel_lsp::parser::keywords::IsReserved(word))
                sites.push_back(WordSite{ line, column, word });

            column += static_cast<uint32_t>(end - i);
            i = end;
        }

        return sites;
    }

    struct Scenario
    {
        const char *name;
        const char *source;
    };

    /**
     * @brief One document per construct, chosen so a missed one is a missed row rather than a
     *        missed line inside a big file nobody reads.
     */
    const std::vector<Scenario> &ColouringScenarios()
    {
        static const std::vector<Scenario> scenarios = {
            { "while and do-while",
              "void Loops()\n"
              "{\n"
              "    while (true) { break; }\n"
              "    do { continue; } while (false);\n"
              "}\n" },

            { "for, with every clause filled in",
              "void Counted(int limit)\n"
              "{\n"
              "    for (int i = 0; i < limit; i++) { }\n"
              "    for (uint j = 0, k = 1; j < 4; j += 1, k *= 2) { }\n"
              "}\n" },

            { "foreach",
              "void Walk(array<int>@ xs)\n"
              "{\n"
              "    foreach (int x : xs) { }\n"
              "}\n" },

            { "switch, case and default",
              "void Pick(int which)\n"
              "{\n"
              "    switch (which)\n"
              "    {\n"
              "        case 1: break;\n"
              "        default: break;\n"
              "    }\n"
              "}\n" },

            { "try and catch",
              "void Guarded()\n"
              "{\n"
              "    try { throwing(); }\n"
              "    catch { }\n"
              "}\n" },

            { "the word operators",
              "bool Decide(bool a, bool b, Thing@ t)\n"
              "{\n"
              "    if (a and b or not a) { }\n"
              "    if (a xor b) { }\n"
              "    if (t is null) { }\n"
              "    return t !is null;\n"
              "}\n" },

            { "an anonymous function inside a block",
              "funcdef void CallbackKind(int v);\n"
              "void Register()\n"
              "{\n"
              "    {\n"
              "        CallbackKind@ cb = function(int v) { return; };\n"
              "    }\n"
              "}\n" },

            { "a class, with modifiers and accessors",
              "shared abstract class Actor\n"
              "{\n"
              "    private int m_health;\n"
              "    protected const bool m_alive = true;\n"
              "    int Health { get const { return m_health; } set { m_health = value; } }\n"
              "    void Hurt(int amount) override { }\n"
              "}\n" },

            { "namespace, enum, interface, mixin, typedef and funcdef",
              "namespace World\n"
              "{\n"
              "    enum Phase { Start = 1, Stop = 2 }\n"
              "    interface Tickable { void Tick(); }\n"
              "    mixin class Helper { void Aid() {} }\n"
              "    typedef double Real;\n"
              "    funcdef void Handler();\n"
              "}\n" },

            { "templates, casts and handles",
              "void Convert(Base@ b)\n"
              "{\n"
              "    array<array<int>> grid;\n"
              "    Derived@ d = cast<Derived>(b);\n"
              "    int64 big = 1;\n"
              "    uint8 small = 2;\n"
              "}\n" },

            { "conditions of every shape",
              "void Conditions(int i, float f, bool flag, Thing@ t)\n"
              "{\n"
              "    if (i > 0 && f <= 1.0f) { }\n"
              "    else if (flag || not flag) { }\n"
              "    else { }\n"
              "    while (i >>> 1 != 0) { i = i >> 1; }\n"
              "}\n" },
        };

        return scenarios;
    }
}

TEST_CASE("SemanticTokensHandler - Every word of the language is classified, in every construct")
{
    // Type_Keyword, Type_Modifier and Type_Type are the three a keyword may legitimately land on -
    // `foreach` is a keyword, `inout` a modifier, `int` a type. Comment and String are here because
    // a reserved word inside a comment or a literal is not a keyword at all, and the scan below
    // finds it anyway; the token covering it says which.
    const std::vector<uint32_t> acceptable = { 15, 16, 1, 17, 18 };

    size_t checked = 0;

    for (const auto &scenario : ColouringScenarios())
    {
        const std::string source = scenario.source;
        const auto swept = SweepTokens(source);
        const auto sites = ReservedWordSites(source);

        INFO("scenario: " << scenario.name);
        REQUIRE_FALSE(sites.empty());

        for (const auto &site : sites)
        {
            INFO("scenario: " << scenario.name << "\nword: " << site.word
                              << " at line " << site.line << " column " << site.column);

            const SweptToken *token = TokenCovering(swept, site.line, site.column);
            REQUIRE(token != nullptr);

            const bool ok = std::find(acceptable.begin(), acceptable.end(), token->type) != acceptable.end();
            INFO("token type: " << token->type);
            CHECK(ok);

            ++checked;
        }
    }

    // The sweep ran over something. Without this the whole case passes when ReservedWordSites
    // stops finding anything - which is the honest failure mode of a scanner written by hand.
    CHECK(checked > 60);
}

TEST_CASE("SemanticTokensHandler - A word operator is a keyword, not punctuation")
{
    // The report: `and` came back as `operator`, which themes paint the colour of plain text, while
    // the textmate grammar had always scoped it keyword.control.conditional. The semantic token was
    // overriding the better answer with a worse one.
    const std::string source =
        "bool Decide(bool a, bool b, Thing@ t)\n"
        "{\n"
        "    if (a and b) { }\n"
        "    if (a or b) { }\n"
        "    if (a xor b) { }\n"
        "    if (not a) { }\n"
        "    if (t is null) { }\n"
        "    return t !is null;\n"
        "}\n";

    const auto swept = SweepTokens(source);

    constexpr uint32_t k_keyword = 15;

    struct Expectation { uint32_t line; const char *needle; };
    const std::vector<Expectation> expectations = {
        { 2, "and" }, { 3, "or" }, { 4, "xor" }, { 5, "not" }, { 6, "is" }, { 7, "!is" },
    };

    for (const auto &expected : expectations)
    {
        INFO("word operator: " << expected.needle);
        const uint32_t column = ColumnOf(source, expected.line, expected.needle);
        const SweptToken *token = TokenAt(swept, expected.line, column);
        REQUIRE(token != nullptr);
        CHECK(token->length == std::string(expected.needle).size());
        CHECK(token->type == k_keyword);
    }
}

TEST_CASE("SemanticTokensHandler - Punctuation operators stay operators")
{
    // The control for the case above. Turning every operator into a keyword would pass it, and
    // would be a worse bug than the one being fixed.
    const std::string source = "void Arithmetic() { int i = 1 + 2 * 3; i += 4; i = i >>> 1; }\n";

    const auto swept = SweepTokens(source);
    constexpr uint32_t k_operator = 21;

    for (const char *needle : { "+ 2", "* 3", "+= 4", ">>> 1" })
    {
        INFO("operator: " << needle);
        const uint32_t column = ColumnOf(source, 0, needle);
        const SweptToken *token = TokenAt(swept, 0, column);
        REQUIRE(token != nullptr);
        CHECK(token->type == k_operator);
    }
}

TEST_CASE("SemanticTokensHandler - Braces and punctuation delimiters never receive operator token type")
{
    const std::string source =
        "void BracketTest(int a, int b)\n"
        "{\n"
        "    int[] arr = { 1, 2 };\n"
        "    if (a > b)\n"
        "    {\n"
        "        arr[0] = (a + b);\n"
        "    }\n"
        "}\n";

    const auto swept = SweepTokens(source);
    constexpr uint32_t k_operator = 21;

    // Check opening and closing braces on lines 1, 2, 4, 6, 7
    for (uint32_t line : { 1u, 2u, 4u, 6u, 7u })
    {
        for (const auto &token : swept)
        {
            if (token.line == line)
            {
                // If any token exists on this line, ensure it is not an operator pointing at a brace or delimiter
                if (token.type == k_operator)
                {
                    // Look at the source slice
                    const std::string lineStr = source.substr(0, source.find('\n', 0));
                    // Check that operator is none of {, }, (, ), [, ], ;, ,
                    FAIL_CHECK("Operator token emitted unexpectedly on line " << line);
                }
            }
        }
    }

    // Verify all tokens across the entire document: no bracket or delimiter can ever be an operator
    std::vector<std::string> lines;
    {
        std::istringstream ss(source);
        std::string line;
        while (std::getline(ss, line))
        {
            lines.push_back(line);
        }
    }

    for (const auto &tok : swept)
    {
        if (tok.type == k_operator)
        {
            REQUIRE(tok.line < lines.size());
            std::string_view slice(lines[tok.line].data() + tok.startCol, tok.length);
            CHECK_FALSE(slice == "{");
            CHECK_FALSE(slice == "}");
            CHECK_FALSE(slice == "(");
            CHECK_FALSE(slice == ")");
            CHECK_FALSE(slice == "[");
            CHECK_FALSE(slice == "]");
            CHECK_FALSE(slice == ";");
            CHECK_FALSE(slice == ",");
        }
    }
}

TEST_CASE("SemanticTokensHandler - Syntax error recovery preserves token integrity and 5-tuple alignment")
{
    // Step 1: Valid function with ternary expression and braces
    const std::string step1Valid =
        "void TestFunction()\n"
        "{\n"
        "    string status = true ? \"READY\" : \"DRYFIRE\";\n"
        "}\n";

    // Step 2: Introduce syntax error (unclosed quote DRYFIRE")
    const std::string step2SyntaxError =
        "void TestFunction()\n"
        "{\n"
        "    string status = true ? \"READY\" : DRYFIRE\";\n"
        "}\n";

    // Step 3: Remove quote back to DRYFIRE
    const std::string step3Recovered =
        "void TestFunction()\n"
        "{\n"
        "    string status = true ? \"READY\" : DRYFIRE;\n"
        "}\n";

    AngelScriptParser parser;
    SymbolTable table;

    // Step 1
    TSTree *tree1 = parser.Parse(step1Valid);
    REQUIRE(tree1 != nullptr);
    CHECK_FALSE(ts_node_has_error(ts_tree_root_node(tree1)));
    SemanticTokensRequest req1{ "file:///test.as", step1Valid, tree1, table };
    const auto tokens1 = GetSemanticTokens(req1);
    ts_tree_delete(tree1);

    REQUIRE(tokens1.data.size() % 5 == 0);
    REQUIRE(!tokens1.data.empty());

    // Step 2
    TSTree *tree2 = parser.Parse(step2SyntaxError);
    REQUIRE(tree2 != nullptr);
    CHECK(ts_node_has_error(ts_tree_root_node(tree2)));
    SemanticTokensRequest req2{ "file:///test.as", step2SyntaxError, tree2, table };
    const auto tokens2 = GetSemanticTokens(req2);
    ts_tree_delete(tree2);

    REQUIRE(tokens2.data.size() % 5 == 0);

    // Step 3
    TSTree *tree3 = parser.Parse(step3Recovered);
    REQUIRE(tree3 != nullptr);
    CHECK_FALSE(ts_node_has_error(ts_tree_root_node(tree3)));
    SemanticTokensRequest req3{ "file:///test.as", step3Recovered, tree3, table };
    const auto tokens3 = GetSemanticTokens(req3);
    ts_tree_delete(tree3);

    REQUIRE(tokens3.data.size() % 5 == 0);
    REQUIRE(!tokens3.data.empty());

    // Assert that { and } have no operator token type assigned
    const auto swept3 = SweepTokens(step3Recovered);
    constexpr uint32_t k_operator = 21;

    for (const auto &tok : swept3)
    {
        if (tok.type == k_operator)
        {
            // Ternary operators ? and : are allowed
            CHECK((tok.line == 2 && (tok.startCol == ColumnOf(step3Recovered, 2, "?") ||
                                     tok.startCol == ColumnOf(step3Recovered, 2, ":"))));
        }
    }

    // Verify { and } on lines 1 and 3 never receive Type_Operator
    const uint32_t openBraceCol = ColumnOf(step3Recovered, 1, "{");
    const SweptToken *openBraceToken = TokenAt(swept3, 1, openBraceCol);
    if (openBraceToken != nullptr)
    {
        CHECK(openBraceToken->type != k_operator);
    }

    const uint32_t closeBraceCol = ColumnOf(step3Recovered, 3, "}");
    const SweptToken *closeBraceToken = TokenAt(swept3, 3, closeBraceCol);
    if (closeBraceToken != nullptr)
    {
        CHECK(closeBraceToken->type != k_operator);
    }

    // Assert that delta edits between step 2 and step 3 strictly obey 5-tuple alignment
    const auto edits = ComputeSemanticTokensDelta(tokens2.data, tokens3.data);
    for (const auto &edit : edits)
    {
        CHECK(edit.start % 5 == 0);
        CHECK(edit.deleteCount % 5 == 0);
        if (edit.data.has_value())
        {
            CHECK(edit.data->size() % 5 == 0);
        }
    }

    // Assert that applying edits reproduces step 3 stream exactly
    auto applied = tokens2.data;
    for (const auto &edit : edits)
    {
        const auto first = applied.begin() + static_cast<std::ptrdiff_t>(edit.start);
        applied.erase(first, first + static_cast<std::ptrdiff_t>(edit.deleteCount));
        if (edit.data.has_value())
        {
            applied.insert(applied.begin() + static_cast<std::ptrdiff_t>(edit.start),
                           edit.data->begin(), edit.data->end());
        }
    }
    CHECK(applied == tokens3.data);
}
