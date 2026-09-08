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

    CHECK(rendered == "> **Note:** Uses `slashForm` and **bold** and *em* and `param.`");
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
