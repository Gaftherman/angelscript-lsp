#include <doctest/doctest.h>

#include "features/definition/DefinitionHandler.h"
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

        std::optional<std::vector<lsp::Location>> DefAt(uint32_t line, uint32_t character)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetDefinition(req);
        }

        std::optional<std::vector<lsp::Location>> TypeDefAt(uint32_t line, uint32_t character)
        {
            DefinitionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
            return GetTypeDefinition(req);
        }
    };
}

TEST_CASE("DefinitionHandler - Go to Definition for Local Variable and Parameter")
{
    std::string code = 
        "void Test(int paramA) {\n"
        "    int localB = 100;\n"
        "    int x = paramA + localB;\n"
        "}\n";

    TestEnvironment env(code);

    // Go to def of paramA on line 2
    auto defParam = env.DefAt(2, 13);
    REQUIRE(defParam.has_value());
    REQUIRE(defParam->size() == 1);
    CHECK((*defParam)[0].range.start.line == 0);

    // Go to def of localB on line 2
    auto defVar = env.DefAt(2, 22);
    REQUIRE(defVar.has_value());
    REQUIRE(defVar->size() == 1);
    CHECK((*defVar)[0].range.start.line == 1);
}

TEST_CASE("DefinitionHandler - Go to Definition for Global Functions and Classes")
{
    std::string code = 
        "class TargetClass {}\n"
        "void TargetFunc() {}\n"
        "void main() {\n"
        "    TargetClass tc;\n"
        "    TargetFunc();\n"
        "}\n";

    TestEnvironment env(code);

    // Go to def of TargetClass on line 3
    auto defClass = env.DefAt(3, 5);
    REQUIRE(defClass.has_value());
    REQUIRE(defClass->size() == 1);
    CHECK((*defClass)[0].range.start.line == 0);

    // Go to def of TargetFunc on line 4
    auto defFunc = env.DefAt(4, 5);
    REQUIRE(defFunc.has_value());
    REQUIRE(defFunc->size() == 1);
    CHECK((*defFunc)[0].range.start.line == 1);
}

TEST_CASE("DefinitionHandler - Go to Definition for Class Member Access")
{
    std::string code = 
        "class Player {\n"
        "    int score;\n"
        "    void Reset() { score = 0; }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.score = 10;\n"
        "    p.Reset();\n"
        "}\n";

    TestEnvironment env(code);

    // Go to def of 'score' in 'p.score' on line 6
    auto defField = env.DefAt(6, 7);
    REQUIRE(defField.has_value());
    REQUIRE(defField->size() == 1);
    CHECK((*defField)[0].range.start.line == 1);

    // Go to def of 'Reset' in 'p.Reset()' on line 7
    auto defMethod = env.DefAt(7, 7);
    REQUIRE(defMethod.has_value());
    REQUIRE(defMethod->size() == 1);
    CHECK((*defMethod)[0].range.start.line == 2);
}

TEST_CASE("DefinitionHandler - Go to Type Definition")
{
    std::string code = 
        "class Monster {}\n"
        "void main() {\n"
        "    Monster@ m = null;\n"
        "}\n";

    TestEnvironment env(code);

    // Go to type def of variable 'm' on line 2
    auto typeDef = env.TypeDefAt(2, 14);
    REQUIRE(typeDef.has_value());
    REQUIRE(typeDef->size() == 1);
    CHECK((*typeDef)[0].range.start.line == 0);
}

TEST_CASE("DefinitionHandler - Invalid Position Returns Nullopt")
{
    TestEnvironment env("void main() {}");
    auto def = env.DefAt(0, 12);
    CHECK(!def.has_value());
}

TEST_CASE("DefinitionHandler - Go to Definition for Class Method and Inherited Method")
{
    std::string code =
        "class Entity {\n"
        "    void TakeDamage(int dmg) {}\n" // line 1
        "}\n"
        "class Player : Entity {\n"
        "    void Attack() {\n"             // line 4
        "        TakeDamage(10);\n"          // line 5, col 9
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Attack();\n"                  // line 10, col 7
        "}\n";

    TestEnvironment env(code);

    // Go to def of 'TakeDamage' in TakeDamage(10) on line 5
    auto defInherited = env.DefAt(5, 10);
    REQUIRE(defInherited.has_value());
    REQUIRE(defInherited->size() == 1);
    CHECK((*defInherited)[0].range.start.line == 1);

    // Go to def of 'Attack' in p.Attack() on line 10
    auto defAttack = env.DefAt(10, 7);
    REQUIRE(defAttack.has_value());
    REQUIRE(defAttack->size() == 1);
    CHECK((*defAttack)[0].range.start.line == 4);
}

TEST_CASE("DefinitionHandler - Go to Definition for Namespace Function")
{
    std::string code =
        "namespace Game {\n"
        "    void Spawn() {}\n" // line 1
        "    void Init() {\n"
        "        Spawn();\n"    // line 3, col 9
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Game::Spawn();\n"  // line 7, col 11
        "}\n";

    TestEnvironment env(code);

    // Go to def of unqualified 'Spawn()' inside namespace on line 3
    auto defInside = env.DefAt(3, 9);
    REQUIRE(defInside.has_value());
    REQUIRE(defInside->size() == 1);
    CHECK((*defInside)[0].range.start.line == 1);

    // Go to def of qualified 'Game::Spawn()' from outside on line 7
    auto defOutside = env.DefAt(7, 11);
    REQUIRE(defOutside.has_value());
    REQUIRE(defOutside->size() == 1);
    CHECK((*defOutside)[0].range.start.line == 1);
}
