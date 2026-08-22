#include <iostream>
#include <string>
#include <doctest/doctest.h>

#include "features/document_highlight/DocumentHighlightHandler.h"
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
        std::string source;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
            : source(code)
        {
            tree = parser.Parse(source);
            symbolCollector.CollectSymbols(uri, source, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(source, parser);
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

        std::optional<DocumentHighlightResult> HighlightsAt(uint32_t line, uint32_t character)
        {
            DocumentHighlightRequest req{
                uri,
                source,
                tree,
                lsp::Position{ line, character },
                symbolTable,
                scopeIndex
            };
            return GetDocumentHighlights(req);
        }
    };
}

TEST_CASE("DocumentHighlight - Local Variables and Parameter Read/Write Classification")
{
    std::string code =
        "void Calculate(int baseValue) {\n"
        "    int multiplier = 2;\n"
        "    int result = baseValue * multiplier;\n"
        "    multiplier = result + multiplier;\n"
        "    multiplier += 1;\n"
        "    multiplier++;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Parameter baseValue highlights: Write at declaration, Read at usage")
    {
        // Cursor on 'baseValue' declaration at line 0, col 20
        auto hls = env.HighlightsAt(0, 20);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        // Declaration is Write
        CHECK((*hls)[0].range.start.line == 0);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Usage is Read
        CHECK((*hls)[1].range.start.line == 2);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }

    SUBCASE("Local variable multiplier highlights: declaration Write, read, assignment LHS Write, compound LHS Write, postfix Write")
    {
        // Cursor on 'multiplier' declaration at line 1, col 10
        auto hls = env.HighlightsAt(1, 10);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 6);

        // 1. Line 1: declaration -> Write
        CHECK((*hls)[0].range.start.line == 1);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // 2. Line 2: baseValue * multiplier -> Read
        CHECK((*hls)[1].range.start.line == 2);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);

        // 3. Line 3 LHS: multiplier = ... -> Write
        CHECK((*hls)[2].range.start.line == 3);
        CHECK((*hls)[2].range.start.character == 4);
        CHECK((*hls)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // 4. Line 3 RHS: result + multiplier -> Read
        CHECK((*hls)[3].range.start.line == 3);
        CHECK((*hls)[3].range.start.character > 15);
        CHECK((*hls)[3].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);

        // 5. Line 4: multiplier += 1 -> Write
        CHECK((*hls)[4].range.start.line == 4);
        CHECK((*hls)[4].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // 6. Line 5: multiplier++ -> Write
        CHECK((*hls)[5].range.start.line == 5);
        CHECK((*hls)[5].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    }
}

TEST_CASE("DocumentHighlight - Member Access Read vs Write")
{
    std::string code =
        "class Entity {\n"
        "    int hp;\n"
        "    void SetHp(int val) {\n"
        "        hp = val;\n"
        "        this.hp = val + 1;\n"
        "    }\n"
        "}\n"
        "void main() {\n"
        "    Entity e;\n"
        "    e.hp = 100;\n"
        "    int current = e.hp;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Highlights on Entity::hp field")
    {
        // Cursor on 'hp' declaration at line 1, col 9
        auto hls = env.HighlightsAt(1, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 5);

        // Line 1: declaration -> Write
        CHECK((*hls)[0].range.start.line == 1);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 3: hp = val -> Write
        CHECK((*hls)[1].range.start.line == 3);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 4: this.hp = val + 1 -> Write
        CHECK((*hls)[2].range.start.line == 4);
        CHECK((*hls)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 9: e.hp = 100 -> Write
        CHECK((*hls)[3].range.start.line == 9);
        CHECK((*hls)[3].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 10: int current = e.hp -> Read
        CHECK((*hls)[4].range.start.line == 10);
        CHECK((*hls)[4].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }

    SUBCASE("Highlights on receiver variable e in e.hp = 100 shows e as Read")
    {
        // Cursor on 'e' in e.hp = 100 at line 9, col 4
        auto hls = env.HighlightsAt(9, 4);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 3);

        // Line 8: Entity e -> Write
        CHECK((*hls)[0].range.start.line == 8);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 9: e.hp = 100 -> e is Read
        CHECK((*hls)[1].range.start.line == 9);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);

        // Line 10: e.hp -> e is Read
        CHECK((*hls)[2].range.start.line == 10);
        CHECK((*hls)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }
}

TEST_CASE("DocumentHighlight - Function & Class Declaration Text Classification")
{
    std::string code =
        "class Player {\n"
        "    void Jump() {}\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.Jump();\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Class Player declaration and type usage are Text")
    {
        // Cursor on 'Player' class declaration line 0, col 6
        auto hls = env.HighlightsAt(0, 6);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        // Line 0: class Player declaration -> Text
        CHECK((*hls)[0].range.start.line == 0);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Write) == lsp::DocumentHighlightKind::Text);

        // Line 4: Player p -> Text
        CHECK((*hls)[1].range.start.line == 4);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Write) == lsp::DocumentHighlightKind::Text);
    }

    SUBCASE("Method Jump declaration is Text and call site is Read")
    {
        // Cursor on 'Jump' declaration line 1, col 9
        auto hls = env.HighlightsAt(1, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        // Line 1: void Jump() declaration -> Text
        CHECK((*hls)[0].range.start.line == 1);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Write) == lsp::DocumentHighlightKind::Text);

        // Line 5: p.Jump() -> Read
        CHECK((*hls)[1].range.start.line == 5);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }
}

TEST_CASE("DocumentHighlight - Lexical Scoping and Shadowing Isolation")
{
    std::string code =
        "void TestShadow() {\n"
        "    int value = 10;\n"
        "    if (true) {\n"
        "        int value = 20;\n"
        "        value += 5;\n"
        "    }\n"
        "    value = value + 1;\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Outer variable highlights only outer occurrences")
    {
        // Cursor on outer 'value' declaration line 1, col 9
        auto hls = env.HighlightsAt(1, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 3);

        // Line 1: int value = 10 -> Write
        CHECK((*hls)[0].range.start.line == 1);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 6 LHS: value = ... -> Write
        CHECK((*hls)[1].range.start.line == 6);
        CHECK((*hls)[1].range.start.character == 4);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 6 RHS: ... + 1 -> Read
        CHECK((*hls)[2].range.start.line == 6);
        CHECK((*hls)[2].range.start.character > 10);
        CHECK((*hls)[2].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }

    SUBCASE("Inner shadowed variable highlights only inner occurrences")
    {
        // Cursor on inner 'value' declaration line 3, col 13
        auto hls = env.HighlightsAt(3, 13);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        // Line 3: int value = 20 -> Write
        CHECK((*hls)[0].range.start.line == 3);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 4: value += 5 -> Write
        CHECK((*hls)[1].range.start.line == 4);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    }
}

TEST_CASE("DocumentHighlight - Out and InOut Parameter Write Classification")
{
    std::string code =
        "void ModifyValues(int &out outVal, int &inout inoutVal, int normalVal) {\n"
        "    outVal = 100;\n"
        "    inoutVal += 50;\n"
        "}\n"
        "void main() {\n"
        "    int a = 0;\n"
        "    int b = 1;\n"
        "    int c = 2;\n"
        "    ModifyValues(a, b, c);\n"
        "}\n";

    TestEnvironment env(code);

    SUBCASE("Argument a passed to &out is classified as Write")
    {
        // Cursor on 'a' declaration at line 5, col 9
        auto hls = env.HighlightsAt(5, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        CHECK((*hls)[0].range.start.line == 5);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 8: ModifyValues(a, b, c) -> a is passed to outVal (&out) -> Write
        CHECK((*hls)[1].range.start.line == 8);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    }

    SUBCASE("Argument b passed to &inout is classified as Write")
    {
        // Cursor on 'b' declaration at line 6, col 9
        auto hls = env.HighlightsAt(6, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        CHECK((*hls)[0].range.start.line == 6);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 8: ModifyValues(a, b, c) -> b is passed to inoutVal (&inout) -> Write
        CHECK((*hls)[1].range.start.line == 8);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);
    }

    SUBCASE("Argument c passed by value is classified as Read")
    {
        // Cursor on 'c' declaration at line 7, col 9
        auto hls = env.HighlightsAt(7, 9);
        REQUIRE(hls.has_value());
        REQUIRE(hls->size() == 2);

        CHECK((*hls)[0].range.start.line == 7);
        CHECK((*hls)[0].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Write);

        // Line 8: ModifyValues(a, b, c) -> c is passed by value -> Read
        CHECK((*hls)[1].range.start.line == 8);
        CHECK((*hls)[1].kind.value_or(lsp::DocumentHighlightKind::Text) == lsp::DocumentHighlightKind::Read);
    }
}

TEST_CASE("DocumentHighlight - Non-Identifier & Edge Cases")
{
    std::string code =
        "// Comment here\n"
        "void main() {\n"
        "    int x = 10;\n"
        "}\n";

    TestEnvironment env(code);

    // Comment at line 0
    auto hlComment = env.HighlightsAt(0, 5);
    CHECK(!hlComment.has_value());

    // Keyword 'void' at line 1, col 2
    auto hlVoid = env.HighlightsAt(1, 2);
    CHECK(!hlVoid.has_value());

    // Keyword 'int' at line 2, col 5
    auto hlInt = env.HighlightsAt(2, 5);
    CHECK(!hlInt.has_value());

    // Out of bounds
    auto hlOob = env.HighlightsAt(100, 100);
    CHECK(!hlOob.has_value());
}
