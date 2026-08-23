#include <doctest/doctest.h>

#include "analysis/DocComment.h"

#include <string>

using namespace angel_lsp::analysis;

TEST_CASE("DocComment - Renders every Doxygen tag a declaration carries")
{
    const std::string source =
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

    const std::string doc = ExtractDocComment(source, 9);
    CHECK(doc.find("Calculates the sum.") != std::string::npos);
    CHECK(doc.find("**Parameters:**") != std::string::npos);
    CHECK(doc.find("`a`: First value.") != std::string::npos);
    CHECK(doc.find("`b`: Second value.") != std::string::npos);
    CHECK(doc.find("**Returns:**") != std::string::npos);
    CHECK(doc.find("> **Note:** Important function.") != std::string::npos);
    CHECK(doc.find("> **Warning:** Use with care.") != std::string::npos);
    CHECK(doc.find("**See also:** OtherFunc") != std::string::npos);
}

TEST_CASE("DocComment - Reads a run of line comments")
{
    const std::string source =
        "/// Spawns the entity.\n"
        "/// Call once per round.\n"
        "void Spawn();\n";

    const std::string doc = ExtractDocComment(source, 2);
    CHECK(doc.find("Spawns the entity.") != std::string::npos);
    CHECK(doc.find("Call once per round.") != std::string::npos);
}

TEST_CASE("DocComment - Skips blank lines between the comment and the declaration")
{
    const std::string source =
        "/// Spawns the entity.\n"
        "\n"
        "\n"
        "void Spawn();\n";

    CHECK(ExtractDocComment(source, 3).find("Spawns the entity.") != std::string::npos);
}

TEST_CASE("DocComment - Returns nothing when there is no comment to read")
{
    SUBCASE("A declaration on the first line has nothing above it")
    {
        CHECK(ExtractDocComment("void Spawn();\n", 0).empty());
    }

    SUBCASE("The line above is ordinary code")
    {
        CHECK(ExtractDocComment("int x = 1;\nvoid Spawn();\n", 1).empty());
    }

    SUBCASE("Empty source")
    {
        CHECK(ExtractDocComment("", 5).empty());
    }

    SUBCASE("A line past the end of the document")
    {
        CHECK(ExtractDocComment("void Spawn();\n", 500).empty());
    }
}
