#include <doctest/doctest.h>

#include "features/implementation/ImplementationHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

// =====================================================================================
// Go to Implementation.
//
// Definition answers "where is this declared". This answers the opposite question - "who answers
// to it" - which is the direction interfaces are actually read in.
// =====================================================================================

namespace
{
    struct Fixture
    {
        AngelScriptParser parser;
        SymbolCollector collector{ nullptr };
        SymbolTable table;
        std::string uri = "file:///impl.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        explicit Fixture(std::string code)
            : sourceCode(std::move(code))
        {
            tree = parser.Parse(sourceCode);
            collector.CollectSymbols(uri, sourceCode, parser, table);
        }

        ~Fixture()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::optional<std::vector<lsp::Location>> At(uint32_t line, uint32_t character)
        {
            const ImplementationRequest request{
                uri, sourceCode, tree, table, lsp::Position{ line, character }
            };
            return GetImplementations(request);
        }
    };

    /** @brief True when some answer starts on this line. */
    bool HasLine(const std::optional<std::vector<lsp::Location>> &locations, uint32_t line)
    {
        return locations.has_value() &&
               std::any_of(locations->begin(), locations->end(), [line](const lsp::Location &location)
               {
                   return location.range.start.line == line;
               });
    }
}

TEST_CASE("Implementation - An interface answers with the classes that implement it")
{
    //  0: interface IThinker
    //  1: {
    //  2:     void Think();
    //  3: }
    //  4: class Robot : IThinker
    //  5: {
    //  6:     void Think() { }
    //  7: }
    //  8: class Human : IThinker
    Fixture fixture(
        "interface IThinker\n"
        "{\n"
        "    void Think();\n"
        "}\n"
        "class Robot : IThinker\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "class Human : IThinker\n"
        "{\n"
        "    void Think() { }\n"
        "}\n");

    const auto locations = fixture.At(0, 12);
    REQUIRE(locations.has_value());
    CHECK(locations->size() == 2);
    CHECK(HasLine(locations, 4));
    CHECK(HasLine(locations, 8));
}

TEST_CASE("Implementation - An interface method answers with the methods that implement it")
{
    Fixture fixture(
        "interface IThinker\n"
        "{\n"
        "    void Think();\n"
        "}\n"
        "class Robot : IThinker\n"
        "{\n"
        "    void Think() { }\n"
        "}\n");

    const auto locations = fixture.At(2, 10);
    REQUIRE(locations.has_value());
    CHECK(HasLine(locations, 6));
}

TEST_CASE("Implementation - A base class answers with what derives from it, transitively")
{
    //  0: class Base { }
    //  1: class Middle : Base { }
    //  2: class Leaf : Middle { }
    Fixture fixture(
        "class Base { }\n"
        "class Middle : Base { }\n"
        "class Leaf : Middle { }\n");

    const auto locations = fixture.At(0, 8);
    REQUIRE(locations.has_value());
    CHECK(locations->size() == 2);
    CHECK(HasLine(locations, 1));
    CHECK(HasLine(locations, 2));
}

TEST_CASE("Implementation - An overridden method answers with its overrides")
{
    //  0: class Base
    //  1: {
    //  2:     void Think() { }
    //  3: }
    //  4: class Derived : Base
    //  5: {
    //  6:     void Think() override { }
    //  7: }
    Fixture fixture(
        "class Base\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "class Derived : Base\n"
        "{\n"
        "    void Think() override { }\n"
        "}\n");

    const auto locations = fixture.At(2, 10);
    REQUIRE(locations.has_value());
    CHECK(HasLine(locations, 6));
}

TEST_CASE("Implementation - A type nothing derives from answers with nothing")
{
    // Not with itself: a list holding only the declaration the cursor is already on makes the
    // editor jump and change nothing, which reads as a bug rather than as an empty answer.
    Fixture fixture("class Lonely { void Think() { } }\n");

    CHECK_FALSE(fixture.At(0, 8).has_value());
}

TEST_CASE("Implementation - A name with no implementations to speak of answers with nothing")
{
    Fixture fixture(
        "int g_count = 0;\n"
        "void Spawn()\n"
        "{\n"
        "    int local = 0;\n"
        "}\n");

    CHECK_FALSE(fixture.At(0, 5).has_value());
    CHECK_FALSE(fixture.At(1, 6).has_value());
    CHECK_FALSE(fixture.At(3, 9).has_value());
}

TEST_CASE("Implementation - A cursor that is not on an identifier answers with nothing")
{
    Fixture fixture("class Base { }\nclass Derived : Base { }\n");

    CHECK_FALSE(fixture.At(0, 0).has_value());
}

TEST_CASE("Implementation - An inherited interface is followed too")
{
    //  0: interface IBase { void Think(); }
    //  1: interface IMiddle : IBase { }
    //  2: class Leaf : IMiddle { void Think() { } }
    Fixture fixture(
        "interface IBase { void Think(); }\n"
        "interface IMiddle : IBase { }\n"
        "class Leaf : IMiddle { void Think() { } }\n");

    const auto locations = fixture.At(0, 12);
    REQUIRE(locations.has_value());
    CHECK(HasLine(locations, 1));
    CHECK(HasLine(locations, 2));
}

TEST_CASE("Implementation - A cycle in the declared bases does not hang the request")
{
    // The class rules report circular inheritance; they do not remove it, and a request arriving
    // while the user is mid-edit has to survive whatever is on screen.
    Fixture fixture(
        "class A : B { }\n"
        "class B : A { }\n");

    const auto locations = fixture.At(0, 6);
    CHECK(locations.has_value());
}
