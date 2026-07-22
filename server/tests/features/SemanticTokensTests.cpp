#include <doctest/doctest.h>
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "document/Document.h"
#include "analysis/SymbolCollector.h"
#include <algorithm>

TEST_SUITE("SemanticTokensTests")
{
    TEST_CASE("ST1: ProvideSemanticTokens returns encoded delta tokens")
    {
        const char *SRC = R"script(
namespace Game
{
    class Player
    {
        int health;
        void Update(float dt)
        {
            health += 10;
        }
    };
}
)script";

        Document doc("file:///semantic.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);

        // Tokens vector should contain 5-tuple deltas
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);
    }

    TEST_CASE("ST2: GetTokenTypesLegend and GetTokenModifiersLegend contain valid LSP strings")
    {
        auto types = angel_lsp::features::SemanticTokensHandler::GetTokenTypesLegend();
        auto modifiers = angel_lsp::features::SemanticTokensHandler::GetTokenModifiersLegend();

        REQUIRE(!types.empty());
        REQUIRE(!modifiers.empty());
        CHECK((std::find(types.begin(), types.end(), "class") != types.end() || std::find(types.begin(), types.end(), "type") != types.end()));
        CHECK(std::find(types.begin(), types.end(), "comment") != types.end());
        CHECK(std::find(types.begin(), types.end(), "string") != types.end());
        CHECK(std::find(modifiers.begin(), modifiers.end(), "declaration") != modifiers.end());
        CHECK(std::find(modifiers.begin(), modifiers.end(), "documentation") != modifiers.end());
    }

    TEST_CASE("ST3: Multi-line comments, strings and numbers semantic tokens")
    {
        const char *SRC = R"script(
/**
 * @brief Player entity
 */
class Player
{
    string name = "Hero";
    int hp = 100;
};
)script";

        Document doc("file:///comments_semantic.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);

        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);
    }

    TEST_CASE("ST4: Primitive types receive Keyword token type")
    {
        const char *SRC = "void func(float x, int y);";
        Document doc("file:///primitives.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);

        // In 5-tuple LSP format: [deltaLine, deltaStartChar, length, tokenType, tokenModifiers]
        // TokenType::Keyword is index 15
        uint32_t keywordTokenType = 15;
        bool foundVoidKeyword = false;
        bool foundFloatKeyword = false;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            uint32_t len = tokens[i + 2];
            uint32_t tType = tokens[i + 3];

            if (len == 4 && tType == keywordTokenType) // "void"
            {
                foundVoidKeyword = true;
            }
            if (len == 5 && tType == keywordTokenType) // "float"
            {
                foundFloatKeyword = true;
            }
        }

        CHECK(foundVoidKeyword);
        CHECK(foundFloatKeyword);
    }

    TEST_CASE("ST5: Multi-line doc comments generate tokens for every line")
    {
        const char *SRC = R"script(/**
 * @param x test
 * @param y test
 */
void test();)script";

        Document doc("file:///multiline_doc.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());

        // Count comment tokens (TokenType::Comment is index 17)
        uint32_t commentTokenType = 17;
        int commentLinesCount = 0;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            if (tokens[i + 3] == commentTokenType)
            {
                commentLinesCount++;
            }
        }

        // Must have 4 comment line tokens for the 4-line doc comment block
        CHECK(commentLinesCount >= 4);
    }

    TEST_CASE("ST6: Preprocessor directive breakdown")
    {
        const char *SRC = R"script(#include "lsp/Server.h"
#ifdef _WIN32
#endif)script";

        Document doc("file:///preproc.as", SRC);
        analysis::SymbolTable table;

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);

        uint32_t keywordTokenType = 15;
        uint32_t stringTokenType = 18;
        uint32_t macroTokenType = 14;

        bool foundIncludeKeyword = false;
        bool foundIncludeString = false;
        bool foundIfdefKeyword = false;
        bool foundIfdefMacro = false;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            uint32_t len = tokens[i + 2];
            uint32_t tType = tokens[i + 3];

            if (len == 8 && tType == keywordTokenType) foundIncludeKeyword = true; // "#include"
            if (len == 14 && tType == stringTokenType) foundIncludeString = true; // "\"lsp/Server.h\""
            if (len == 6 && tType == keywordTokenType) foundIfdefKeyword = true; // "#ifdef"
            if (len == 6 && tType == macroTokenType) foundIfdefMacro = true; // "_WIN32"
        }

        CHECK(foundIncludeKeyword);
        CHECK(foundIncludeString);
        CHECK(foundIfdefKeyword);
        CHECK(foundIfdefMacro);
    }
}
