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

    TEST_CASE("ST4: Dynamic AST symbols receive semantic tokens")
    {
        const char *SRC = "void func(float x, int y);";
        Document doc("file:///primitives.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);

        // TokenType::Function is index 12, TokenType::Parameter is index 7
        uint32_t funcTokenType = 12;
        uint32_t paramTokenType = 7;
        bool foundFunc = false;
        bool foundParam = false;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            uint32_t len = tokens[i + 2];
            uint32_t tType = tokens[i + 3];

            if (len == 4 && tType == funcTokenType) // "func"
            {
                foundFunc = true;
            }
            if (len == 1 && tType == paramTokenType) // "x" or "y"
            {
                foundParam = true;
            }
        }

        CHECK(foundFunc);
        CHECK(foundParam);
    }

    TEST_CASE("ST5: Preprocessor macro receives Macro semantic token")
    {
        const char *SRC = R"script(#include "lsp/Server.h"
#ifdef _WIN32
#endif)script";

        Document doc("file:///preproc.as", SRC);
        analysis::SymbolTable table;

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);

        uint32_t macroTokenType = 14;
        bool foundIfdefMacro = false;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            uint32_t len = tokens[i + 2];
            uint32_t tType = tokens[i + 3];

            if (len == 6 && tType == macroTokenType) foundIfdefMacro = true; // "_WIN32"
        }

        CHECK(foundIfdefMacro);
    }

    TEST_CASE("ST6: Mirror script main.as tokenization test")
    {
        const char *SRC = R"script(
#include "Engine.h"

#ifdef _WIN32
    // Windows platform configuration
#endif

namespace Engine
{
    /** @brief 3D Vector structure */
    class Vector3
    {
        float x;
        float y;
        float z;

        Vector3() {}
        Vector3(float ax, float ay, float az) {}
        ~Vector3() {}

        float Length() const
        {
            return x + y + z;
        }
    };

    enum State
    {
        STATE_IDLE,
        STATE_RUNNING
    };
}

void ProcessData(Engine::Vector3 pos, int count, string name)
{
    Engine::State currentState = Engine::STATE_RUNNING;
    if (count > 0 && currentState == Engine::STATE_RUNNING)
    {
        float len = pos.Length();
    }
}

void Main()
{
    Engine::Vector3 playerPos(10.0f, 20.0f, 0.0f);
    ProcessData(playerPos, 5, "Hero");
}
)script";

        Document doc("file:///main.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);
    }

    TEST_CASE("ST7: Complex nested namespaces, class methods, arrays and operators test")
    {
        const char *SRC = R"script(
namespace Engine
{
    namespace Graphics
    {
        class Texture
        {
            private string m_name;
            Texture(const string &in name)
            {
                m_name = name;
            }
            const string& GetName() const
            {
                return m_name;
            }
        }
    }
}

void TestComplex()
{
    Engine::Graphics::Texture@ tex = Engine::Graphics::Texture("wood.png");
    if (tex is not null)
    {
        string name = tex.GetName();
    }
}
)script";

        Document doc("file:///complex_nested.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);

        // Verify Texture is recognized as Class (TokenType::Class = 2)
        uint32_t classTokenType = 2;
        bool foundClass = false;

        for (size_t i = 0; i < tokens.size(); i += 5)
        {
            uint32_t len = tokens[i + 2];
            uint32_t tType = tokens[i + 3];

            if (len == 7 && tType == classTokenType) // "Texture"
            {
                foundClass = true;
            }
        }

        CHECK(foundClass);
    }

    TEST_CASE("ST8: Lambda expression and preprocessor semantic tokenization")
    {
        const char *SRC = R"script(
#if ACTIVE
        auto adder = function(int a, int b) { return a + b; };
#endif
)script";

        Document doc("file:///lambda_preproc.as", SRC);
        analysis::SymbolTable table;
        analysis::SymbolCollector::CollectGlobals(doc, table);
        analysis::SymbolCollector::TraverseLocals(doc.RootNode(), doc, table, nullptr);

        std::vector<uint32_t> tokens = angel_lsp::features::SemanticTokensHandler::ProvideSemanticTokens(doc, table);
        REQUIRE(!tokens.empty());
        CHECK(tokens.size() % 5 == 0);
    }
}
