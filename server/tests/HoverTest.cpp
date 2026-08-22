#include <doctest/doctest.h>

#include "features/hover/HoverHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<lsp::Hover> HoverAt(uint32_t line, uint32_t character)
        {
            HoverRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetHover(req);
        }
    };
}

TEST_CASE("HoverHandler - ExtractDocComment parsing")
{
    std::string source = 
        "/**\n"
        " * @brief Calculates the sum.\n"
        " * @param a First value.\n"
        " * @param b Second value.\n"
        " * @return The sum.\n"
        " * @note Important function.\n"
        " * @warning Use with care.\n"
        " * @see OtherFunc\n"
        " */\n"
        "int Add(int a, int b);\n";

    std::string doc = ExtractDocComment(source, 9);
    CHECK(doc.find("Calculates the sum.") != std::string::npos);
    CHECK(doc.find("**Parameters:**") != std::string::npos);
    CHECK(doc.find("`a`: First value.") != std::string::npos);
    CHECK(doc.find("`b`: Second value.") != std::string::npos);
    CHECK(doc.find("**Returns:**") != std::string::npos);
    CHECK(doc.find("> **Note:** Important function.") != std::string::npos);
    CHECK(doc.find("> **Warning:** Use with care.") != std::string::npos);
    CHECK(doc.find("**See also:** OtherFunc") != std::string::npos);
}

TEST_CASE("HoverHandler - Primitive Type Hover")
{
    TestEnvironment env("void main() { int x = 42; }");
    auto hover = env.HoverAt(0, 15); // 'int'
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("(primitive type) int") != std::string::npos);
}

TEST_CASE("HoverHandler - Local Variable and Parameter Hover")
{
    TestEnvironment env("void Foo(float speed) { int count = 10; count = 20; }");
    
    // Hover on 'speed' at column 15
    auto hoverParam = env.HoverAt(0, 15);
    REQUIRE(hoverParam.has_value());
    auto contentParam = std::get<lsp::MarkupContent>(hoverParam->contents);
    CHECK(contentParam.value.find("(parameter) float speed") != std::string::npos);

    // Hover on 'count' at column 42
    auto hoverVar = env.HoverAt(0, 42);
    REQUIRE(hoverVar.has_value());
    auto contentVar = std::get<lsp::MarkupContent>(hoverVar->contents);
    CHECK(contentVar.value.find("(local variable) int count") != std::string::npos);
}

TEST_CASE("HoverHandler - Global Function Hover with Overloads")
{
    std::string code = 
        "/// Calculates distance.\n"
        "float Dist(float x, float y) { return 0.0f; }\n"
        "float Dist(float x, float y, float z) { return 0.0f; }\n"
        "void main() { Dist(1.0, 2.0); }\n";

    TestEnvironment env(code);
    auto hover = env.HoverAt(3, 15); // 'Dist'
    REQUIRE(hover.has_value());
    auto content = std::get<lsp::MarkupContent>(hover->contents);
    CHECK(content.value.find("float Dist(float x, float y)") != std::string::npos);
    CHECK(content.value.find("float Dist(float x, float y, float z)") != std::string::npos);
    CHECK(content.value.find("Calculates distance.") != std::string::npos);
}

TEST_CASE("HoverHandler - Class and Member Hover")
{
    std::string code =
        "class Player : BaseEntity {\n"
        "    int health;\n"
        "    void Attack(int dmg) {}\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack(10);\n"
        "}\n";

    TestEnvironment env(code);

    // Hover on class name 'Player' on line 5
    auto hoverClass = env.HoverAt(5, 5);
    REQUIRE(hoverClass.has_value());
    auto contentClass = std::get<lsp::MarkupContent>(hoverClass->contents);
    CHECK(contentClass.value.find("class Player : BaseEntity") != std::string::npos);

    // Hover on member method 'Attack' on line 6
    auto hoverMethod = env.HoverAt(6, 7);
    REQUIRE(hoverMethod.has_value());
    auto contentMethod = std::get<lsp::MarkupContent>(hoverMethod->contents);
    CHECK(contentMethod.value.find("void Player::Attack(int dmg)") != std::string::npos);
}

TEST_CASE("HoverHandler - Invalid Position Returns Nullopt")
{
    TestEnvironment env("void main() { }");
    auto hover = env.HoverAt(0, 14); // inside whitespace
    CHECK(!hover.has_value());
}
