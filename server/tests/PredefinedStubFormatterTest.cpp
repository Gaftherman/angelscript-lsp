#include <doctest/doctest.h>

#include <string>

#include "features/formatting/PredefinedStubFormatter.h"
#include "parser/AngelScriptParser.h"

using angel_lsp::features::formatting::FormatPredefinedStub;

namespace
{
    std::string Format(const std::string &source)
    {
        angel_lsp::parser::AngelScriptParser parser(nullptr);
        return FormatPredefinedStub(source, parser);
    }
}

// =====================================================================================
// Gathering a namespace a generated stub scattered.
//
// A stub written from an engine's registration table emits one declaration per registered entity,
// so a namespace with twenty members arrives as twenty namespaces. Sven Co-op's own stub has
// `Schedules` twenty times and `Hooks::Player` seventeen. The analyzer reads that perfectly well;
// nobody else can.
// =====================================================================================

TEST_CASE("PredefinedStubFormatter - Gathers repeated namespaces into one block")
{
    const std::string source =
        "//Empty string. Useful when a reference to a string is needed.;\n"
        "namespace String { const string EMPTY_STRING; }\n"
        "//Default comparison type.;\n"
        "namespace String { const CompareType DEFAULT_COMPARE; }\n";

    const std::string expected =
        "//Empty string. Useful when a reference to a string is needed.;\n"
        "namespace String\n"
        "{\n"
        "\tconst string EMPTY_STRING;\n"
        "\t//Default comparison type.;\n"
        "\tconst CompareType DEFAULT_COMPARE;\n"
        "}\n";

    CHECK(Format(source) == expected);
}

TEST_CASE("PredefinedStubFormatter - Every comment keeps the declaration it describes")
{
    // The load-bearing property. A stub's comments are its documentation - they are what hover
    // shows - so a formatter that moved one onto the wrong member would be worse than none.
    const std::string source =
        "//first;\n"
        "namespace N { int A; }\n"
        "//second;\n"
        "namespace N { int B; }\n"
        "//third;\n"
        "namespace N { int C; }\n";

    const std::string formatted = Format(source);

    const size_t first = formatted.find("//first;");
    const size_t a = formatted.find("int A;");
    const size_t second = formatted.find("//second;");
    const size_t b = formatted.find("int B;");
    const size_t third = formatted.find("//third;");
    const size_t c = formatted.find("int C;");

    INFO("formatted:\n" << formatted);
    REQUIRE(first != std::string::npos);
    REQUIRE(c != std::string::npos);

    // Each comment still immediately precedes its own member, and the order is the source's.
    CHECK(first < a);
    CHECK(a < second);
    CHECK(second < b);
    CHECK(b < third);
    CHECK(third < c);
}

TEST_CASE("PredefinedStubFormatter - Two namespaces of different names stay apart")
{
    const std::string source =
        "namespace A { int X; }\n"
        "namespace B { int Y; }\n"
        "namespace A { int Z; }\n";

    const std::string formatted = Format(source);
    INFO("formatted:\n" << formatted);

    // A is merged at its first position, so it comes before B, and B is untouched.
    CHECK(formatted.find("namespace A") < formatted.find("namespace B"));
    CHECK(formatted.find("int X;") < formatted.find("int Z;"));
    CHECK(formatted.find("int Z;") < formatted.find("namespace B"));

    // B had nothing to merge, and still comes out a block: a file where some namespaces are
    // blocks and others are one-liners is not formatted.
    CHECK(formatted.find("namespace B\n{\n\tint Y;\n}") != std::string::npos);
}

TEST_CASE("PredefinedStubFormatter - A qualified name is one namespace, not two")
{
    // `Hooks::Player` and `Hooks::Game` are different namespaces and merging by the first segment
    // would put one's members inside the other.
    const std::string source =
        "namespace Hooks::Player { int P1; }\n"
        "namespace Hooks::Game { int G1; }\n"
        "namespace Hooks::Player { int P2; }\n";

    const std::string formatted = Format(source);
    INFO("formatted:\n" << formatted);

    CHECK(formatted.find("namespace Hooks::Player") != std::string::npos);
    CHECK(formatted.find("namespace Hooks::Game\n{\n\tint G1;\n}") != std::string::npos);
    CHECK(formatted.find("int P1;") < formatted.find("int P2;"));
    CHECK(formatted.find("int P2;") < formatted.find("namespace Hooks::Game"));
}

TEST_CASE("PredefinedStubFormatter - A file with no namespace at all is byte for byte the same")
{
    // A formatter the user runs on the wrong file must be a no-op rather than a rewrite, and the
    // check that it produced nothing new is what the server reports back as "already formatted".
    const std::string source =
        "//A class;\n"
        "class CThing\n"
        "{\n"
        "\tvoid Go();\n"
        "}\n"
        "funcdef void Callback();\n"
        "enum EThing { ONE, TWO }\n";

    CHECK(Format(source) == source);
}

TEST_CASE("PredefinedStubFormatter - A namespace already written as a block is left as it was")
{
    const std::string source =
        "namespace Already\n"
        "{\n"
        "\t//A member;\n"
        "\tint Member;\n"
        "}\n";

    CHECK(Format(source) == source);
}

TEST_CASE("PredefinedStubFormatter - Formatting twice changes nothing the second time")
{
    const std::string source =
        "//one;\n"
        "namespace N { int A; }\n"
        "//two;\n"
        "namespace N { int B; }\n";

    const std::string once = Format(source);
    CHECK(Format(once) == once);
}

TEST_CASE("PredefinedStubFormatter - A file that does not parse is left alone")
{
    // Declaration boundaries in a broken file are guesses, and moving text on a guess is how a
    // formatter eats someone's work.
    const std::string source =
        "namespace N { int A; }\n"
        "class Broken { void Go(   \n"
        "namespace N { int B; }\n";

    CHECK(Format(source) == source);
}

TEST_CASE("PredefinedStubFormatter - Declarations that are not namespaces are untouched")
{
    const std::string source =
        "//The engine's own;\n"
        "class CBaseEntity\n"
        "{\n"
        "\tvoid Spawn();\n"
        "}\n"
        "namespace S { int A; }\n"
        "enum EThing { ONE, TWO }\n"
        "namespace S { int B; }\n";

    const std::string formatted = Format(source);
    INFO("formatted:\n" << formatted);

    CHECK(formatted.find("//The engine's own;\nclass CBaseEntity\n{\n\tvoid Spawn();\n}") != std::string::npos);
    CHECK(formatted.find("enum EThing { ONE, TWO }") != std::string::npos);
}
