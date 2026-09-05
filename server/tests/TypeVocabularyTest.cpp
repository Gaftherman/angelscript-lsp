#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include "analysis/SemanticHelpers.h"
#include "parser/Keywords.h"
#include "parser/Primitives.h"

using namespace angel_lsp;

// =====================================================================================
// The vocabulary the analyzer used to keep in nine places: three keyword lists, five primitive
// lists, two copies of the template-argument splitter and eight of LastScopeSegment.
//
// Consolidating them is only safe if what they answered is written down, so this is that. The
// splitter cases are the ones that mattered: its two callers disagree about an empty argument, and
// merging them without noticing would have changed which template argument lists count as usable.
// =====================================================================================

TEST_CASE("Vocabulary - a reserved word can never be a name, a contextual one can")
{
    // Measured against the compiler, one file per word: `void t() { int <word>; }`.
    for (const std::string_view reserved : { "int", "class", "return", "foreach", "using", "xor" })
    {
        CAPTURE(reserved);
        CHECK(parser::keywords::IsReserved(reserved));
        CHECK(parser::keywords::IsKeyword(reserved));
    }

    // `int final;` and `int get;` both compile. These may colour and complete, never reject.
    for (const std::string_view contextual : { "final", "get", "set", "shared", "this", "super" })
    {
        CAPTURE(contextual);
        CHECK_FALSE(parser::keywords::IsReserved(contextual));
        CHECK(parser::keywords::IsKeyword(contextual));
    }

    // `string` is a type the application registers, not a word of the language. `with` is
    // JavaScript's and appears nowhere in this grammar.
    CHECK_FALSE(parser::keywords::IsKeyword("string"));
    CHECK_FALSE(parser::keywords::IsKeyword("with"));
}

TEST_CASE("Vocabulary - the primitive subsets are subsets")
{
    for (const std::string_view name : parser::primitives::k_integers)
    {
        CAPTURE(name);
        CHECK(parser::primitives::IsNumeric(name));
        CHECK(parser::primitives::IsPrimitive(name));
        CHECK(parser::primitives::IsNonNullable(name));
    }

    for (const std::string_view name : parser::primitives::k_floats)
    {
        CAPTURE(name);
        CHECK(parser::primitives::IsNumeric(name));
        CHECK(parser::primitives::IsPrimitive(name));
    }

    // `void` is the one primitive that holds nothing, which is the whole distinction.
    CHECK(parser::primitives::IsPrimitive("void"));
    CHECK_FALSE(parser::primitives::IsNonNullable("void"));
    CHECK_FALSE(parser::primitives::IsNumeric("bool"));
    CHECK(parser::primitives::IsNonNullable("bool"));

    // `auto` is not a primitive. IsCorePrimitive adds it deliberately and says so.
    CHECK_FALSE(parser::primitives::IsPrimitive("auto"));
    CHECK(analysis::IsCorePrimitive("auto"));
}

TEST_CASE("Vocabulary - the template argument splitter counts nesting, not commas")
{
    struct Case
    {
        std::string_view inner;
        std::vector<std::string> expected;
    };

    const Case cases[] = {
        { "int",                       { "int" } },
        { "int, float",                { "int", "float" } },
        { " int , float ",             { "int", "float" } },
        // The comma inside the inner list belongs to it: three arguments, not four.
        { "int, array<int, float>",    { "int", "array<int, float>" } },
        { "array<array<int>>, string", { "array<array<int>>", "string" } },
        // Empties are kept by the splitter; what to do with them is each caller's business.
        { "int,",                      { "int", "" } },
        { "int,,float",                { "int", "", "float" } },
        { "",                          { "" } },
    };

    for (const Case &c : cases)
    {
        CAPTURE(c.inner);
        CHECK(analysis::SplitTemplateArguments(c.inner) == c.expected);
    }
}

TEST_CASE("Vocabulary - a qualification does not hide a name")
{
    CHECK(analysis::LastScopeSegment("Foo") == "Foo");
    CHECK(analysis::LastScopeSegment("NS::Foo") == "Foo");
    CHECK(analysis::LastScopeSegment("a::b::c::Foo") == "Foo");
    CHECK(analysis::LastScopeSegment("") == "");

    // Not a qualification: a single colon is left alone.
    CHECK(analysis::LastScopeSegment("a:b") == "a:b");
}
