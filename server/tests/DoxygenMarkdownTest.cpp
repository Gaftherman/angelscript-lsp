#include <doctest/doctest.h>

#include "analysis/DoxygenMarkdown.h"

#include <string>

using namespace angel_lsp::analysis;

TEST_CASE("DoxygenMarkdown - Worked example matches clangd canonical format exactly")
{
    const std::string input =
        "/**\n"
        " * @brief Computes the hash of a buffer.\n"
        " * Murmur3 is applied when \\c len is greater than 64 bytes.\n"
        " * @tparam T Underlying buffer type.\n"
        " * @param[in] data Pointer to the start of the memory.\n"
        " * @param[out] err_code Error code on failure.\n"
        " * @return \\b 0 on success, or a negative error code.\n"
        " * @warning Do not pass null pointers.\n"
        " */";

    const std::string expected =
        "Computes the hash of a buffer.\n\n"
        "Murmur3 is applied when `len` is greater than 64 bytes.\n\n"
        "* `T`: Underlying buffer type.\n\n"
        "* `data` *(in)*: Pointer to the start of the memory.\n"
        "* `err_code` *(out)*: Error code on failure.\n\n"
        "**Returns:** **0** on success, or a negative error code.\n\n"
        "> **Warning:** Do not pass null pointers.";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Implicit brief without @brief tag")
{
    const std::string input =
        "/**\n"
        " * Implicit brief ends here. And this is body text.\n"
        " */";

    const std::string expected =
        "Implicit brief ends here.\n\n"
        "And this is body text.";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Triple-slash run normalizes to equivalent block comment")
{
    const std::string tripleSlash =
        "/// @brief Computes the hash of a buffer.\n"
        "/// Murmur3 is applied when \\c len is greater than 64 bytes.\n"
        "/// @param[in] data Pointer to the start of the memory.\n"
        "/// @return \\b 0 on success, or a negative error code.\n";

    const std::string blockComment =
        "/**\n"
        " * @brief Computes the hash of a buffer.\n"
        " * Murmur3 is applied when \\c len is greater than 64 bytes.\n"
        " * @param[in] data Pointer to the start of the memory.\n"
        " * @return \\b 0 on success, or a negative error code.\n"
        " */";

    const std::string renderedSlash = RenderDoxygenMarkdown(tripleSlash);
    const std::string renderedBlock = RenderDoxygenMarkdown(blockComment);

    CHECK(renderedSlash == renderedBlock);
    CHECK(!renderedSlash.empty());
}

TEST_CASE("DoxygenMarkdown - Inline formatting commands merge into preceding block")
{
    const std::string input =
        "/**\n"
        " * @note Uses @c slashForm and \\b bold and \\e em and \\p param.\n"
        " */";

    const std::string rendered = RenderDoxygenMarkdown(input);

    CHECK(rendered == "> **Note:** Uses `slashForm` and **bold** and *em* and `param`.");
    CHECK(rendered.find("> **B:**") == std::string::npos);
    CHECK(rendered.find("> **E:**") == std::string::npos);
    CHECK(rendered.find("> **P:**") == std::string::npos);
    CHECK(rendered.find("> **C:**") == std::string::npos);
}

TEST_CASE("DoxygenMarkdown - Parameter direction in,out survives syntax error")
{
    const std::string input =
        "/**\n"
        " * @param[in,out] buffer Buffer to process.\n"
        " */";

    const std::string rendered = RenderDoxygenMarkdown(input);
    CHECK(rendered == "* `buffer` *(in,out)*: Buffer to process.");
    CHECK(rendered.find("*(in,out)*") != std::string::npos);
}

TEST_CASE("DoxygenMarkdown - Comma-separated parameter names emit multiple bullets")
{
    const std::string input =
        "/**\n"
        " * @param a, b Shared description.\n"
        " */";

    const std::string expected =
        "* `a`: Shared description.\n"
        "* `b`: Shared description.";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Multi-line parameter description joins cleanly")
{
    const std::string input =
        "/**\n"
        " * @param data A description that\n"
        " * continues onto a second line.\n"
        " */";

    const std::string expected =
        "* `data`: A description that continues onto a second line.";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Code block with language fences cleanly")
{
    const std::string input =
        "/**\n"
        " * An example:\n"
        " * @code{.cpp}\n"
        " * int x = 1;\n"
        " * int y = 2;\n"
        " * @endcode\n"
        " */";

    const std::string expected =
        "An example:\n\n"
        "```cpp\n"
        "int x = 1;\n"
        "int y = 2;\n"
        "```";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Unknown tag surfaces as capitalized admonition")
{
    const std::string input =
        "/**\n"
        " * @customthing Some custom explanation.\n"
        " */";

    CHECK(RenderDoxygenMarkdown(input) == "> **Customthing:** Some custom explanation.");
}

TEST_CASE("DoxygenMarkdown - Empty and whitespace comments return empty string")
{
    CHECK(RenderDoxygenMarkdown("").empty());
    CHECK(RenderDoxygenMarkdown("   \n \t  ").empty());
    CHECK(RenderDoxygenMarkdown("/** */").empty());
    CHECK(RenderDoxygenMarkdown("/***/").empty());
}

TEST_CASE("DoxygenMarkdown - First line without period preserves subsequent @param tag")
{
    const std::string input =
        "/**\n"
        " * Does a thing\n"
        " * @param a Value.\n"
        " * @return Nothing.\n"
        " */";

    const std::string expected =
        "Does a thing\n\n"
        "* `a`: Value.\n\n"
        "**Returns:** Nothing.";

    const std::string rendered = RenderDoxygenMarkdown(input);
    CHECK(rendered == expected);
    CHECK(rendered.find("* `a`: Value.") != std::string::npos);
}

TEST_CASE("DoxygenMarkdown - Structural tags (@class, @struct, @fn, @file) are omitted from hover")
{
    SUBCASE("Bare @class produces empty markdown")
    {
        const std::string input =
            "/**\n"
            " * @class Player\n"
            " */";

        CHECK(RenderDoxygenMarkdown(input).empty());
    }

    SUBCASE("@class with attached description preserves description as body without @class tag")
    {
        const std::string input =
            "/**\n"
            " * @class Player\n"
            " * Controls player movement and state.\n"
            " */";

        const std::string rendered = RenderDoxygenMarkdown(input);
        CHECK(rendered == "Controls player movement and state.");
        CHECK(rendered.find("Class") == std::string::npos);
    }

    SUBCASE("Structural tags @struct, @file, @fn are stripped without generating admonitions")
    {
        const std::string input =
            "/**\n"
            " * @struct Transform2D\n"
            " * Represents a 2D coordinate transform.\n"
            " * @fn void UpdateTransform()\n"
            " * @file MathUtils.as\n"
            " */";

        const std::string rendered = RenderDoxygenMarkdown(input);
        CHECK(rendered == "Represents a 2D coordinate transform.");
        CHECK(rendered.find("Struct") == std::string::npos);
        CHECK(rendered.find("Fn") == std::string::npos);
        CHECK(rendered.find("File") == std::string::npos);
    }
}

TEST_CASE("DoxygenMarkdown - @details produces a body paragraph, not an admonition")
{
    const std::string input =
        "/**\n"
        " * @brief Short summary.\n"
        " * @details Extended detailed explanation across\n"
        " * multiple lines.\n"
        " */";

    const std::string expected =
        "Short summary.\n\n"
        "Extended detailed explanation across multiple lines.";

    CHECK(RenderDoxygenMarkdown(input) == expected);
    CHECK(RenderDoxygenMarkdown(input).find("Details:") == std::string::npos);
}

TEST_CASE("DoxygenMarkdown - @retval renders bullets under Returns section")
{
    SUBCASE("@retval alongside @return")
    {
        const std::string input =
            "/**\n"
            " * @brief Executes a task.\n"
            " * @return Exit status code.\n"
            " * @retval 0 Success.\n"
            " * @retval -1 Generic error.\n"
            " */";

        const std::string expected =
            "Executes a task.\n\n"
            "**Returns:** Exit status code.\n"
            "* `0`: Success.\n"
            "* `-1`: Generic error.";

        CHECK(RenderDoxygenMarkdown(input) == expected);
    }

    SUBCASE("@retval without @return synthesizes Returns header")
    {
        const std::string input =
            "/**\n"
            " * @brief Checks validity.\n"
            " * @retval true Valid.\n"
            " * @retval false Invalid.\n"
            " */";

        const std::string expected =
            "Checks validity.\n\n"
            "**Returns:**\n"
            "* `true`: Valid.\n"
            "* `false`: Invalid.";

        CHECK(RenderDoxygenMarkdown(input) == expected);
    }
}

TEST_CASE("DoxygenMarkdown - @ref formats as inline code")
{
    const std::string input =
        "/**\n"
        " * See @ref Actor for the base class.\n"
        " */";

    CHECK(RenderDoxygenMarkdown(input) == "See `Actor` for the base class.");
}

TEST_CASE("DoxygenMarkdown - Inline command trailing and leading punctuation")
{
    SUBCASE("Trailing periods, commas, colons, and parens remain outside code delimiters")
    {
        const std::string input =
            "/**\n"
            " * Returns @c true. Also (@c value), check @b status: ok!\n"
            " */";

        const std::string expected =
            "Returns `true`.\n\n"
            "Also (`value`), check **status**: ok!";

        CHECK(RenderDoxygenMarkdown(input) == expected);
    }
}

TEST_CASE("DoxygenMarkdown - @verbatim renders as an unfenced code block")
{
    const std::string input =
        "/**\n"
        " * @verbatim\n"
        " * raw ASCII art or text\n"
        " * line two\n"
        " * @endverbatim\n"
        " */";

    const std::string expected =
        "```\n"
        "raw ASCII art or text\n"
        "line two\n"
        "```";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Basic HTML tags convert to Markdown")
{
    const std::string input =
        "/**\n"
        " * Uses <code>int</code>, <b>bold text</b>, and <i>italic text</i>.\n"
        " */";

    CHECK(RenderDoxygenMarkdown(input) == "Uses `int`, **bold text**, and *italic text*.");
}

TEST_CASE("DoxygenMarkdown - Doxygen -# numbered list converts to ordered markdown")
{
    const std::string input =
        "/**\n"
        " * -# First step\n"
        " * -# Second step\n"
        " */";

    const std::string expected =
        "1. First step\n"
        "2. Second step";

    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Doxygen escape sequences unescape cleanly")
{
    const std::string input =
        "/**\n"
        " * Contact at \\@admin or use \\$variable and \\\\backslash.\n"
        " */";

    CHECK(RenderDoxygenMarkdown(input) == "Contact at @admin or use $variable and \\backslash.");
}

TEST_CASE("DoxygenMarkdown - Trailing semicolon after sentence-ending dot is not emitted as separate paragraph")
{
    const std::string input = "//Persistence object id type.;";
    CHECK(RenderDoxygenMarkdown(input) == "Persistence object id type.");
}

TEST_CASE("DoxygenMarkdown - Literal newline escape splits into paragraphs without creating NIf admonition")
{
    const std::string input = "// Persistence object id type.\\nIf foo is true: do bar.";
    const std::string expected =
        "Persistence object id type.\n\n"
        "If foo is true: do bar.";
    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Literal newline at start does not create NIf tag")
{
    const std::string input = "/// \\nIf condition is true: proceed.";
    CHECK(RenderDoxygenMarkdown(input) == "If condition is true: proceed.");
}

TEST_CASE("DoxygenMarkdown - Numbered item in brief description does not truncate at digit dot")
{
    const std::string input = "/// 1. Initialize subsystem.\\n2. Run processing loop.";
    const std::string expected =
        "1. Initialize subsystem.\n\n"
        "2. Run processing loop.";
    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Trailing semicolon after sentence dot strips semicolon from remainder")
{
    const std::string input = "/// First sentence.; Second sentence.";
    const std::string expected =
        "First sentence.\n\n"
        "Second sentence.";
    CHECK(RenderDoxygenMarkdown(input) == expected);
}

TEST_CASE("DoxygenMarkdown - Literal CRLF newlines split cleanly without underflow")
{
    const std::string input = "/// Line 1.\\r\\n\\r\\nLine 2.";
    const std::string expected =
        "Line 1.\n\n"
        "Line 2.";
    CHECK(RenderDoxygenMarkdown(input) == expected);
}

