#include <doctest/doctest.h>

#include "features/linked_editing/LinkedEditingRangeHandler.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Linked editing.
//
// The client retypes every range it is handed as the user types, with no further round trip, so a
// range set that is not the whole story silently desynchronises a project. That is why only locals
// and parameters are answered: a lexical scope cannot escape its document.
// =====================================================================================

namespace
{
    struct Fixture
    {
        AngelScriptParser parser;
        LocalScopeCollector collector{ nullptr };
        std::string sourceCode;
        std::shared_ptr<Scope> scopeRoot;

        explicit Fixture(std::string code)
            : sourceCode(std::move(code))
        {
            scopeRoot = collector.CollectScopes(sourceCode, parser);
        }

        std::optional<lsp::LinkedEditingRanges> At(uint32_t line, uint32_t character)
        {
            const LinkedEditingRangeRequest request{
                sourceCode, scopeRoot.get(), lsp::Position{ line, character }
            };
            return GetLinkedEditingRanges(request);
        }
    };
}

TEST_CASE("LinkedEditing - Links a local's declaration and its uses")
{
    //  0: void main()
    //  1: {
    //  2:     int ticks = 0;
    //  3:     ticks = ticks + 1;
    //  4: }
    Fixture fixture(
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "    ticks = ticks + 1;\n"
        "}\n");

    const auto ranges = fixture.At(2, 9);
    REQUIRE(ranges.has_value());
    CHECK(ranges->ranges.size() >= 3);
}

TEST_CASE("LinkedEditing - Works from a use as well as from the declaration")
{
    Fixture fixture(
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "    ticks = ticks + 1;\n"
        "}\n");

    const auto ranges = fixture.At(3, 5);
    REQUIRE(ranges.has_value());
    CHECK(ranges->ranges.size() >= 3);
}

TEST_CASE("LinkedEditing - Links a parameter")
{
    Fixture fixture(
        "void Spawn(int count)\n"
        "{\n"
        "    count = count + 1;\n"
        "}\n");

    const auto ranges = fixture.At(0, 16);
    REQUIRE(ranges.has_value());
    CHECK(ranges->ranges.size() >= 3);
}

TEST_CASE("LinkedEditing - A shadowed name in an inner scope is not dragged in")
{
    //  0: void main()
    //  1: {
    //  2:     int ticks = 0;
    //  3:     {
    //  4:         int ticks = 1;
    //  5:         ticks = 2;
    //  6:     }
    //  7:     ticks = 3;
    //  8: }
    //
    // The inner block declares its own `ticks`. Retyping the outer one must not touch it, and this
    // is exactly the case a naive text search gets wrong.
    Fixture fixture(
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "    {\n"
        "        int ticks = 1;\n"
        "        ticks = 2;\n"
        "    }\n"
        "    ticks = 3;\n"
        "}\n");

    const auto ranges = fixture.At(2, 9);
    REQUIRE(ranges.has_value());
    for (const auto &range : ranges->ranges)
    {
        INFO("line ", range.start.line);
        CHECK((range.start.line == 2 || range.start.line == 7));
    }
}

TEST_CASE("LinkedEditing - A global is not offered")
{
    // It can be referenced from another file in the same module, and retyping only this file's
    // occurrences would leave the rest behind. Rename looks across documents; this cannot.
    Fixture fixture(
        "int g_count = 0;\n"
        "void main() { g_count = g_count + 1; }\n");

    CHECK_FALSE(fixture.At(0, 5).has_value());
}

TEST_CASE("LinkedEditing - A function name is not offered")
{
    Fixture fixture(
        "void Spawn() { }\n"
        "void main() { Spawn(); Spawn(); }\n");

    CHECK_FALSE(fixture.At(0, 6).has_value());
}

TEST_CASE("LinkedEditing - A declaration with no uses is not offered")
{
    // One range gives an editor nothing it did not already have.
    Fixture fixture(
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "}\n");

    CHECK_FALSE(fixture.At(2, 9).has_value());
}

TEST_CASE("LinkedEditing - A cursor on nothing named is not offered")
{
    Fixture fixture(
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "    ticks = ticks + 1;\n"
        "}\n");

    CHECK_FALSE(fixture.At(1, 0).has_value());
}

TEST_CASE("LinkedEditing - A document with no scope tree is not offered")
{
    const std::string source = "void main() { }\n";
    const LinkedEditingRangeRequest request{ source, nullptr, lsp::Position{ 0, 6 } };
    CHECK_FALSE(GetLinkedEditingRanges(request).has_value());
}
