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
    CHECK(doc.find("* `a`: First value.") != std::string::npos);
    CHECK(doc.find("* `b`: Second value.") != std::string::npos);
    CHECK(doc.find("**Returns:** The sum.") != std::string::npos);
    CHECK(doc.find("> **Note:** Important function.") != std::string::npos);
    CHECK(doc.find("> **Warning:** Use with care.") != std::string::npos);
    CHECK(doc.find("> **See also:** OtherFunc") != std::string::npos);
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

TEST_CASE("DocComment - Reads trailing comments on declaration line")
{
    SUBCASE("Line 0 declaration with trailing comment")
    {
        const std::string source = "ref(); // asBEHAVE_CONSTRUCT;\n";
        CHECK(ExtractDocComment(source, 0) == "asBEHAVE_CONSTRUCT;");
    }

    SUBCASE("Later line declaration with trailing comment")
    {
        const std::string source = "class Entity {\n    Entity(); // asBEHAVE_CONSTRUCT;\n};\n";
        CHECK(ExtractDocComment(source, 1) == "asBEHAVE_CONSTRUCT;");
    }

    SUBCASE("Trailing block comment on declaration line")
    {
        const std::string source = "void Spawn(); /* Spawns the entity */\n";
        CHECK(ExtractDocComment(source, 0) == "Spawns the entity");
    }

    SUBCASE("Ignores comment syntax inside string literals")
    {
        const std::string source = "void Log(string s = \"// not a comment\"); // Real doc comment\n";
        CHECK(ExtractDocComment(source, 0) == "Real doc comment");
    }

    SUBCASE("Ignores internal list pattern marker")
    {
        const std::string source = "array(int &in) {repeat T}; //@listpattern {repeat T}\n";
        CHECK(ExtractDocComment(source, 0).empty());
    }

    SUBCASE("Strips internal list pattern marker following real trailing comment")
    {
        const std::string sourceWithoutSemi = "array(int &in) {repeat T}; // asBEHAVE_LIST_FACTORY//@listpattern {repeat T}\n";
        CHECK(ExtractDocComment(sourceWithoutSemi, 0) == "asBEHAVE_LIST_FACTORY");

        const std::string sourceWithSemi = "array(int &in) {repeat T}; // asBEHAVE_LIST_FACTORY;//@listpattern {repeat T}\n";
        CHECK(ExtractDocComment(sourceWithSemi, 0) == "asBEHAVE_LIST_FACTORY;");
    }

    SUBCASE("Isolates block comment when trailing code exists on same line")
    {
        const std::string source = "void Spawn(); /* Spawns the entity */ int unused = 0;\n";
        CHECK(ExtractDocComment(source, 0) == "Spawns the entity");
    }
}

