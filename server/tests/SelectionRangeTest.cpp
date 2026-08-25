#include <doctest/doctest.h>

#include "features/selection_range/SelectionRangeHandler.h"
#include "parser/AngelScriptParser.h"

#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::parser;

// =====================================================================================
// Expand selection.
//
// The one feature that needs nothing but the parse: the editor walks outward one syntactic step
// at a time, and the tree already knows what those steps are.
// =====================================================================================

namespace
{
    struct Fixture
    {
        AngelScriptParser parser;
        std::string sourceCode;
        TSTree *tree = nullptr;

        explicit Fixture(std::string code)
            : sourceCode(std::move(code))
        {
            tree = parser.Parse(sourceCode);
        }

        ~Fixture()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::vector<lsp::SelectionRange> At(std::vector<lsp::Position> positions)
        {
            const SelectionRangeRequest request{ sourceCode, tree, positions };
            return GetSelectionRanges(request);
        }
    };

    /** @brief Flattens one chain into its ranges, innermost first. */
    std::vector<lsp::Range> Chain(const lsp::SelectionRange &head)
    {
        std::vector<lsp::Range> ranges;
        for (const lsp::SelectionRange *link = &head; link != nullptr; link = link->parent.get())
        {
            ranges.push_back(link->range);
        }
        return ranges;
    }

    /** @brief True when the outer range fully contains the inner one. */
    bool Contains(const lsp::Range &outer, const lsp::Range &inner)
    {
        const bool startsBefore = outer.start.line < inner.start.line ||
                                  (outer.start.line == inner.start.line &&
                                   outer.start.character <= inner.start.character);
        const bool endsAfter = outer.end.line > inner.end.line ||
                               (outer.end.line == inner.end.line &&
                                outer.end.character >= inner.end.character);
        return startsBefore && endsAfter;
    }
}

TEST_CASE("SelectionRange - Each link contains the one before it")
{
    // The protocol's only hard requirement, and the one a client will misbehave on: parent.range
    // must contain this.range.
    Fixture fixture(
        "class Entity\n"
        "{\n"
        "    void Think()\n"
        "    {\n"
        "        int ticks = 0;\n"
        "    }\n"
        "}\n");

    const auto results = fixture.At({ lsp::Position{ 4, 12 } });
    REQUIRE(results.size() == 1);

    const auto ranges = Chain(results[0]);
    REQUIRE(ranges.size() >= 3);
    for (size_t i = 1; i < ranges.size(); ++i)
    {
        INFO("link ", i);
        CHECK(Contains(ranges[i], ranges[i - 1]));
    }
}

TEST_CASE("SelectionRange - The innermost link is the token under the cursor")
{
    Fixture fixture("void Think() { int ticks = 0; }\n");

    const auto results = fixture.At({ lsp::Position{ 0, 20 } });
    REQUIRE(results.size() == 1);

    const auto ranges = Chain(results[0]);
    REQUIRE_FALSE(ranges.empty());
    CHECK(ranges.front().start.line == 0);
    CHECK(ranges.front().start.character == 19);
    CHECK(ranges.front().end.character == 24);
}

TEST_CASE("SelectionRange - The chain reaches the whole document")
{
    Fixture fixture("void Think() { int ticks = 0; }\n");

    const auto results = fixture.At({ lsp::Position{ 0, 20 } });
    REQUIRE(results.size() == 1);

    const auto ranges = Chain(results[0]);
    REQUIRE_FALSE(ranges.empty());
    CHECK(ranges.back().start.line == 0);
    CHECK(ranges.back().start.character == 0);
}

TEST_CASE("SelectionRange - No two consecutive links offer the same range")
{
    // A chain that repeats a range makes the expand keystroke look broken: the user presses it and
    // the selection does not move.
    Fixture fixture(
        "class Entity\n"
        "{\n"
        "    int health;\n"
        "}\n");

    const auto results = fixture.At({ lsp::Position{ 2, 9 } });
    REQUIRE(results.size() == 1);

    const auto ranges = Chain(results[0]);
    for (size_t i = 1; i < ranges.size(); ++i)
    {
        INFO("link ", i);
        const bool identical = ranges[i].start.line == ranges[i - 1].start.line &&
                               ranges[i].start.character == ranges[i - 1].start.character &&
                               ranges[i].end.line == ranges[i - 1].end.line &&
                               ranges[i].end.character == ranges[i - 1].end.character;
        CHECK_FALSE(identical);
    }
}

TEST_CASE("SelectionRange - Every requested position is answered, in order")
{
    // The client lines the results up with what it sent by index, so a skipped position is worse
    // than a useless one.
    Fixture fixture("void Think() { int ticks = 0; }\n");

    const auto results = fixture.At({
        lsp::Position{ 0, 5 },
        lsp::Position{ 0, 20 },
        lsp::Position{ 0, 26 }
    });

    REQUIRE(results.size() == 3);
    CHECK(Chain(results[0]).front().start.character == 5);
    CHECK(Chain(results[1]).front().start.character == 19);
}

TEST_CASE("SelectionRange - A position past the end still gets an answer")
{
    Fixture fixture("void Think() { }\n");

    const auto results = fixture.At({ lsp::Position{ 99, 99 } });
    REQUIRE(results.size() == 1);
    CHECK_FALSE(Chain(results[0]).empty());
}

TEST_CASE("SelectionRange - A document with no tree yields nothing")
{
    const std::string source = "void Think() { }\n";
    const std::vector<lsp::Position> positions{ lsp::Position{ 0, 5 } };
    const SelectionRangeRequest request{ source, nullptr, positions };

    CHECK(GetSelectionRanges(request).empty());
}
