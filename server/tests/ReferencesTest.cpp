#include <iostream>
#include <string>
#include <doctest/doctest.h>

#include "features/references/ReferencesHandler.h"
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
    struct MultiFileTestEnv
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, TSTree *> trees;

        void AddFile(const std::string &uri, const std::string &code)
        {
            sources[uri] = code;
            TSTree *tree = parser.Parse(code);
            trees[uri] = tree;
            symbolCollector.CollectSymbols(uri, code, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(code, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~MultiFileTestEnv()
        {
            for (auto &[uri, tree] : trees)
            {
                if (tree)
                {
                    ts_tree_delete(tree);
                }
            }
        }

        std::optional<ReferencesResult> RefsAt(const std::string &uri, uint32_t line, uint32_t character, bool includeDecl = true)
        {
            ReferencesRequest req{
                uri,
                sources[uri],
                trees[uri],
                lsp::Position{ line, character },
                includeDecl,
                symbolTable,
                scopeIndex
            };
            return GetReferences(req);
        }
    };

    struct TestEnvironment
    {
        MultiFileTestEnv multiEnv;
        std::string uri = "file:///test.as";

        TestEnvironment(const std::string &code)
        {
            multiEnv.AddFile(uri, code);
        }

        std::optional<ReferencesResult> RefsAt(uint32_t line, uint32_t character, bool includeDecl = true)
        {
            return multiEnv.RefsAt(uri, line, character, includeDecl);
        }
    };
}

TEST_CASE("ReferencesHandler - Local Variable and Parameter References")
{
    std::string code =
        "void Calculate(int baseValue) {\n"
        "    int multiplier = 2;\n"
        "    int result = baseValue * multiplier;\n"
        "    multiplier = result + multiplier;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Find references of parameter baseValue with includeDeclaration=true")
    {
        // Cursor on parameter declaration 'baseValue' at line 0, col 20
        auto refs = env.RefsAt(0, 20, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 0); // Declaration
        CHECK((*refs)[1].range.start.line == 2); // Usage
    }

    SUBCASE("Find references of parameter baseValue with includeDeclaration=false")
    {
        // Cursor on parameter usage 'baseValue' at line 2, col 18
        auto refs = env.RefsAt(2, 18, false);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 1);
        CHECK((*refs)[0].range.start.line == 2);
    }

    SUBCASE("Find references of local variable multiplier with includeDeclaration=true")
    {
        // 'multiplier' declared on line 1, used on line 2, line 3 (lhs), line 3 (rhs)
        auto refs = env.RefsAt(1, 10, true);
        REQUIRE(refs.has_value());
        CHECK(refs->size() == 4);
        CHECK((*refs)[0].range.start.line == 1); // Declaration
        CHECK((*refs)[1].range.start.line == 2); // Line 2 usage
        CHECK((*refs)[2].range.start.line == 3); // Line 3 usage (LHS)
        CHECK((*refs)[3].range.start.line == 3); // Line 3 usage (RHS)
    }

    SUBCASE("Find references of local variable multiplier with includeDeclaration=false")
    {
        auto refs = env.RefsAt(2, 30, false);
        REQUIRE(refs.has_value());
        CHECK(refs->size() == 3);
        for (const auto &loc : *refs)
        {
            CHECK(loc.range.start.line > 1);
        }
    }
}

TEST_CASE("ReferencesHandler - Lexical Scope Shadowing Isolation")
{
    std::string code =
        "void TestScope() {\n"
        "    int value = 10;\n"
        "    if (true) {\n"
        "        int value = 20;\n"
        "        value += 5;\n"
        "    }\n"
        "    value += 1;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Outer variable references exclude shadowed inner scope")
    {
        // Cursor on outer 'value' at line 1, col 9
        auto refs = env.RefsAt(1, 9, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 1); // Outer declaration
        CHECK((*refs)[1].range.start.line == 6); // Outer usage (value += 1)
    }

    SUBCASE("Inner variable references include only inner scope")
    {
        // Cursor on inner 'value' at line 3, col 13
        auto refs = env.RefsAt(3, 13, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 3); // Inner declaration
        CHECK((*refs)[1].range.start.line == 4); // Inner usage (value += 5)
    }
}

TEST_CASE("ReferencesHandler - Class Field and Method References")
{
    std::string code =
        "class Hero {\n"
        "    int health = 100;\n"
        "    void Heal(int amount) {\n"
        "        health += amount;\n"
        "        this.health += 1;\n"
        "    }\n"
        "}\n"
        "class Villain {\n"
        "    int health = 50;\n"
        "}\n"
        "void main() {\n"
        "    Hero h;\n"
        "    h.health = 200;\n"
        "    h.Heal(50);\n"
        "    Villain v;\n"
        "    v.health = 0;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("References to Hero::health correctly finds field usages and ignores Villain::health")
    {
        // Cursor on Hero::health declaration at line 1, col 9
        auto refs = env.RefsAt(1, 9, true);
        REQUIRE(refs.has_value());
        // Hero::health decl (line 1), unqualified in Heal (line 3), this.health in Heal (line 4), h.health in main (line 12)
        REQUIRE(refs->size() == 4);
        CHECK((*refs)[0].range.start.line == 1);
        CHECK((*refs)[1].range.start.line == 3);
        CHECK((*refs)[2].range.start.line == 4);
        CHECK((*refs)[3].range.start.line == 12);
    }

    SUBCASE("References to Hero::Heal finds declaration and method calls")
    {
        // Cursor on Hero::Heal at line 2, col 10
        auto refs = env.RefsAt(2, 10, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 2);  // Declaration
        CHECK((*refs)[1].range.start.line == 13); // Call site: h.Heal(50)
    }

    SUBCASE("References to Villain::health isolated to Villain")
    {
        // Cursor on Villain::health declaration at line 8, col 9
        auto refs = env.RefsAt(8, 9, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 8);  // Declaration
        CHECK((*refs)[1].range.start.line == 15); // v.health = 0
    }
}

TEST_CASE("ReferencesHandler - Class Inheritance Hierarchy References")
{
    std::string code =
        "class Entity {\n"
        "    int id = 0;\n"
        "    void Reset() { id = 0; }\n"
        "}\n"
        "class Actor : Entity {\n"
        "    void Act() { Reset(); }\n"
        "}\n"
        "void main() {\n"
        "    Actor a;\n"
        "    a.Reset();\n"
        "}\n";

    TestEnvironment env(code);

    // References to Entity::Reset() from declaration at line 2, col 10
    auto refs = env.RefsAt(2, 10, true);
    REQUIRE(refs.has_value());
    // Entity::Reset decl (line 2), unqualified in Actor::Act (line 5), a.Reset() in main (line 9)
    REQUIRE(refs->size() == 3);
    CHECK((*refs)[0].range.start.line == 2);
    CHECK((*refs)[1].range.start.line == 5);
    CHECK((*refs)[2].range.start.line == 9);
}

TEST_CASE("ReferencesHandler - Global Symbols and Cross-File References")
{
    MultiFileTestEnv env;
    std::string fileA = "file:///a_math.as";
    std::string codeA =
        "int GlobalCounter = 0;\n"
        "void IncrementCounter() {\n"
        "    GlobalCounter += 1;\n"
        "}\n";

    std::string fileB = "file:///b_main.as";
    std::string codeB =
        "void main() {\n"
        "    IncrementCounter();\n"
        "    GlobalCounter += 10;\n"
        "}\n"
        "void ShadowTest() {\n"
        "    int GlobalCounter = 99;\n"
        "    GlobalCounter += 1;\n"
        "}\n";

    env.AddFile(fileA, codeA);
    env.AddFile(fileB, codeB);

    SUBCASE("Find references of GlobalCounter across files with local shadow protection")
    {
        // Cursor on GlobalCounter declaration in a_math.as line 0, col 5
        auto refs = env.RefsAt(fileA, 0, 5, true);
        REQUIRE(refs.has_value());
        // fileA: decl (line 0), usage (line 2); fileB: usage in main (line 2). Shadowed in ShadowTest excluded!
        REQUIRE(refs->size() == 3);
        CHECK((*refs)[0].uri.toString() == fileA);
        CHECK((*refs)[0].range.start.line == 0);
        CHECK((*refs)[1].uri.toString() == fileA);
        CHECK((*refs)[1].range.start.line == 2);
        CHECK((*refs)[2].uri.toString() == fileB);
        CHECK((*refs)[2].range.start.line == 2);
    }

    SUBCASE("Find references of IncrementCounter across files")
    {
        // Cursor on IncrementCounter call in b_main.as line 1, col 6
        auto refs = env.RefsAt(fileB, 1, 6, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].uri.toString() == fileA);
        CHECK((*refs)[0].range.start.line == 1); // Declaration in a_math.as
        CHECK((*refs)[1].uri.toString() == fileB);
        CHECK((*refs)[1].range.start.line == 1); // Call site in b_main.as
    }
}

TEST_CASE("ReferencesHandler - Enum and Enum Member References")
{
    std::string code =
        "enum GameState {\n"
        "    State_Idle,\n"
        "    State_Running\n"
        "}\n"
        "void main() {\n"
        "    GameState state = State_Idle;\n"
        "    if (state == State_Idle) {}\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Find references of enum member State_Idle")
    {
        // Cursor on State_Idle declaration at line 1, col 5
        auto refs = env.RefsAt(1, 5, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 3);
        CHECK((*refs)[0].range.start.line == 1); // Decl
        CHECK((*refs)[1].range.start.line == 5); // Usage 1
        CHECK((*refs)[2].range.start.line == 6); // Usage 2
    }

    SUBCASE("Find references of enum type GameState")
    {
        // Cursor on GameState declaration at line 0, col 6
        auto refs = env.RefsAt(0, 6, true);
        REQUIRE(refs.has_value());
        REQUIRE(refs->size() == 2);
        CHECK((*refs)[0].range.start.line == 0); // Decl
        CHECK((*refs)[1].range.start.line == 5); // Type usage
    }
}

TEST_CASE("ReferencesHandler - Edge Cases & Non-Identifiers")
{
    std::string code =
        "// Some comment\n"
        "void main() {\n"
        "    int x = 10;\n"
        "}\n";

    TestEnvironment env(code);

    // Cursor on comment line 0
    auto refsComment = env.RefsAt(0, 5);
    CHECK(!refsComment.has_value());

    // Cursor on keyword 'void' line 1, col 1
    auto refsVoid = env.RefsAt(1, 1);
    CHECK(!refsVoid.has_value());

    // Cursor on keyword 'int' line 2, col 5
    auto refsInt = env.RefsAt(2, 5);
    CHECK(!refsInt.has_value());
}

TEST_CASE("ReferencesHandler - Class Method References Across Inheritance")
{
    std::string code =
        "class Entity {\n"
        "    void TakeDamage(int dmg) {}\n" // line 1
        "}\n"
        "class Player : Entity {\n"
        "    void Attack() {\n"
        "        TakeDamage(5);\n"           // line 5
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.TakeDamage(10);\n"            // line 10
        "}\n";

    TestEnvironment env(code);

    // References of TakeDamage with includeDeclaration=true
    auto refs = env.RefsAt(1, 10, true);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 3);
    CHECK((*refs)[0].range.start.line == 1);  // Decl
    CHECK((*refs)[1].range.start.line == 5);  // Sibling call
    CHECK((*refs)[2].range.start.line == 10); // Member call
}

TEST_CASE("ReferencesHandler - Namespace Function References")
{
    std::string code =
        "namespace Game {\n"
        "    void Spawn() {}\n" // line 1
        "    void Init() {\n"
        "        Spawn();\n"    // line 3
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Game::Spawn();\n"  // line 7
        "}\n";

    TestEnvironment env(code);

    // References of Spawn declaration with includeDeclaration=true
    auto refs = env.RefsAt(1, 9, true);
    REQUIRE(refs.has_value());
    REQUIRE(refs->size() == 3);
    CHECK((*refs)[0].range.start.line == 1);
    CHECK((*refs)[1].range.start.line == 3);
    CHECK((*refs)[2].range.start.line == 7);
}
