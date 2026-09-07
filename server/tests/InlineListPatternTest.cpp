#include <doctest/doctest.h>

#include <string>

#include "analysis/ListPattern.h"

using angel_lsp::analysis::FindListPatternTag;
using angel_lsp::analysis::RewriteInlineListPatterns;

// =====================================================================================
// A stub writing its list factory the way the AngelScript manual does.
//
//     array(int &in type, int &in list) {repeat T};   // asBEHAVE_LIST_FACTORY
//
// Measured before writing any of this: the notation is documentation, not script syntax. The
// oracle rejects it, and this server's own parser reads `{repeat T}` as a statement block declaring
// a variable `T` of type `repeat` - `Syntax error: missing ';'` and `Unknown type 'repeat'`.
//
// A stub is never compiled by AngelScript, so it is allowed to spell things the language does not.
// What it is not allowed to do is reach the parser that way, which is what this rewrite is for.
// =====================================================================================

TEST_CASE("InlineListPattern - The pattern becomes a tag the collector already reads")
{
    const std::string source =
        "class array<T>\n"
        "{\n"
        "\tarray();\n"
        "\tarray(int &in type, int &in list) {repeat T};\n"
        "}\n";

    const std::string rewritten = RewriteInlineListPatterns(source);
    INFO("rewritten:\n" << rewritten);

    CHECK(rewritten.find("//@listpattern {repeat T}") != std::string::npos);

    // And what the parser now sees is an ordinary declaration.
    CHECK(rewritten.find("array(int &in type, int &in list)") != std::string::npos);
    CHECK(rewritten.find("{repeat T};") == std::string::npos);
}

TEST_CASE("InlineListPattern - Every line and column of the declarations survives")
{
    // The load-bearing property, and the reason the pattern is blanked rather than deleted: a stub
    // is a file the user navigates, and positions on the wire are line and column. A deletion would
    // pull the `;` left and send go-to-definition to the wrong column on that line.
    //
    // Byte OFFSETS after the rewritten line do move, by the length of the appended comment, and
    // that is fine: the tree and the text the collector reads are both the rewritten one, so they
    // agree with each other. Nothing compares an offset against the file on disk.
    const std::string source =
        "class array<T>\n"
        "{\n"
        "\tarray(int &in type, int &in list) {repeat T};\n"
        "\tuint length() const;\n"
        "}\n";

    const std::string rewritten = RewriteInlineListPatterns(source);

    // Line and column of a marker, which is what a position on the wire is made of.
    const auto positionOf = [](const std::string &text, const std::string &needle)
    {
        const size_t at = text.find(needle);
        REQUIRE(at != std::string::npos);
        const size_t lineStart = text.rfind('\n', at);
        return std::pair<size_t, size_t>(
            static_cast<size_t>(std::count(text.begin(), text.begin() + at, '\n')),
            at - (lineStart == std::string::npos ? 0 : lineStart + 1));
    };

    CHECK(std::count(rewritten.begin(), rewritten.end(), '\n') ==
          std::count(source.begin(), source.end(), '\n'));

    CHECK(positionOf(rewritten, "array(int") == positionOf(source, "array(int"));
    CHECK(positionOf(rewritten, "uint length") == positionOf(source, "uint length"));

    // Including the `;` that ends the rewritten declaration itself. Looked up from the start of
    // that line rather than by searching for ";\n", because in the rewritten text the comment now
    // follows it - which is exactly the shift this test exists to bound to that one line.
    const auto semicolonColumn = [](const std::string &text)
    {
        const size_t lineAt = text.find("\tarray(int");
        REQUIRE(lineAt != std::string::npos);
        return text.find(';', lineAt) - lineAt;
    };
    CHECK(semicolonColumn(rewritten) == semicolonColumn(source));
}

TEST_CASE("InlineListPattern - The dictionary's nested pattern survives its inner braces")
{
    const std::string source =
        "class dictionary\n"
        "{\n"
        "\tdictionary(int &in type, int &in list) {repeat {string, ?}};\n"
        "}\n";

    const std::string rewritten = RewriteInlineListPatterns(source);
    INFO("rewritten:\n" << rewritten);

    CHECK(rewritten.find("//@listpattern {repeat {string, ?}}") != std::string::npos);
}

TEST_CASE("InlineListPattern - A file without one is returned unchanged")
{
    const std::string source =
        "class CThing\n"
        "{\n"
        "\tvoid Go();\n"
        "}\n"
        "namespace N { int A; }\n";

    CHECK(RewriteInlineListPatterns(source) == source);
}

TEST_CASE("InlineListPattern - A declaration with a body is left alone")
{
    // `) {` is the signal, and a definition has one too. A stub has no definitions, but the
    // built-in profiles pass through here as well and nothing may eat a body.
    const std::string source =
        "void Helper() { int x = 1; }\n";

    CHECK(RewriteInlineListPatterns(source) == source);
}

TEST_CASE("InlineListPattern - The tag is found inside the class, not only above it")
{
    // The reading half. FindListPatternTag used to search only the lines above a declaration,
    // which is where the `///` form sits; the rewritten inline form lands on a member line.
    const std::string rewritten = RewriteInlineListPatterns(
        "class array<T>\n"
        "{\n"
        "\tarray(int &in type, int &in list) {repeat T};\n"
        "}\n");

    CHECK(FindListPatternTag(rewritten, 0) == "{repeat T}");
}

TEST_CASE("InlineListPattern - One class cannot read the next one's pattern")
{
    const std::string rewritten = RewriteInlineListPatterns(
        "class NoPattern\n"
        "{\n"
        "\tvoid Go();\n"
        "}\n"
        "class HasPattern\n"
        "{\n"
        "\tHasPattern(int &in type, int &in list) {repeat T};\n"
        "}\n");

    CHECK(FindListPatternTag(rewritten, 0).empty());
    CHECK(FindListPatternTag(rewritten, 4) == "{repeat T}");
}

TEST_CASE("InlineListPattern - The doc-comment form still works")
{
    // The existing notation is not replaced, only joined. Every stub already written keeps working.
    const std::string source =
        "/// @listpattern {repeat T}\n"
        "class array<T>\n"
        "{\n"
        "\tarray();\n"
        "}\n";

    CHECK(RewriteInlineListPatterns(source) == source);
    CHECK(FindListPatternTag(source, 1) == "{repeat T}");
}
