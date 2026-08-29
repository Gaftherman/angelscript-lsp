#include <doctest/doctest.h>

#include "utils/PreprocessorRegions.h"

#include <string>

// =====================================================================================
// `#if` / `#endif` exclusion.
//
// AngelScript has no preprocessor; `#if` is handled by the CScriptBuilder add-on and its model is
// deliberately tiny (scriptbuilder.cpp, LoadScriptSection / ExcludeCode):
//
//   - Only `#if <identifier>` and `#endif`. No #else, no #elif, no #ifdef, no #define.
//   - The identifier is not evaluated. It is looked up in a set the *host application* fills via
//     DefineWord(), and a block whose word is absent is blanked out before compilation.
//   - Nesting is tracked, so an inner `#if` does not close an outer excluded block early.
//   - Blanking preserves newlines, so line numbers never shift.
//
// This matters because analysing text the compiler never sees produces diagnostics about code that
// does not exist. It was found by comparing against the real compiler: AS-Harness's json.as keeps
// deliberately-unbuildable code inside `#if FALSE`, and every diagnostic we reported for that file
// came from that block.
// =====================================================================================

using namespace angel_lsp::utils;

namespace
{
    bool Excludes(const std::string &source, uint32_t line,
                  const ankerl::unordered_dense::set<std::string> &defined = {})
    {
        return IsLineExcluded(FindExcludedLineRanges(source, defined), line);
    }
}

TEST_CASE("Preprocessor - An undefined word excludes the whole block")
{
    const std::string source =
        "void before() {}\n"   // 0
        "#if FALSE\n"          // 1
        "void inside() {}\n"   // 2
        "#endif\n"             // 3
        "void after() {}\n";   // 4

    CHECK_FALSE(Excludes(source, 0));
    CHECK(Excludes(source, 1));   // the directive line itself is blanked too
    CHECK(Excludes(source, 2));
    CHECK(Excludes(source, 3));
    CHECK_FALSE(Excludes(source, 4));
}

TEST_CASE("Preprocessor - A defined word keeps the block")
{
    const std::string source =
        "#if DEBUG_BUILD\n"
        "void inside() {}\n"
        "#endif\n";

    ankerl::unordered_dense::set<std::string> defined;
    defined.insert("DEBUG_BUILD");

    CHECK_FALSE(Excludes(source, 1, defined));

    // ...and is excluded again the moment the host stops defining it.
    CHECK(Excludes(source, 1));
}

TEST_CASE("Preprocessor - Nested directives do not end an excluded block early")
{
    const std::string source =
        "#if OUTER\n"          // 0
        "void a() {}\n"        // 1
        "#if INNER\n"          // 2
        "void b() {}\n"        // 3
        "#endif\n"             // 4  closes INNER, NOT OUTER
        "void c() {}\n"        // 5  still inside OUTER
        "#endif\n"             // 6  closes OUTER
        "void d() {}\n";       // 7

    for (uint32_t line = 0; line <= 6; ++line)
    {
        CAPTURE(line);
        CHECK(Excludes(source, line));
    }
    CHECK_FALSE(Excludes(source, 7));
}

TEST_CASE("Preprocessor - An unterminated block runs to end of file")
{
    const std::string source =
        "void before() {}\n"
        "#if FALSE\n"
        "void inside() {}\n"
        "void alsoInside() {}\n";

    CHECK_FALSE(Excludes(source, 0));
    CHECK(Excludes(source, 2));
    CHECK(Excludes(source, 3));
}

TEST_CASE("Preprocessor - Directives inside comments and strings are not directives")
{
    // A '#if' that is not really a directive must not silently blank out the rest of the file,
    // which is the failure mode a naive line scan would have.
    const std::string commented =
        "/*\n"
        "#if FALSE\n"
        "*/\n"
        "void real() {}\n";
    CHECK_FALSE(Excludes(commented, 3));

    const std::string lineComment =
        "// #if FALSE\n"
        "void real() {}\n";
    CHECK_FALSE(Excludes(lineComment, 1));

    const std::string inString =
        "string s = \"#if FALSE\";\n"
        "void real() {}\n";
    CHECK_FALSE(Excludes(inString, 1));
}

TEST_CASE("Preprocessor - A stray endif and a bare if are harmless")
{
    // Neither is something CScriptBuilder acts on, and neither may take the rest of the file with
    // it - a malformed directive that blanked out a whole document would be far worse than one
    // that is ignored.
    CHECK_FALSE(Excludes("#endif\nvoid real() {}\n", 1));
    CHECK_FALSE(Excludes("#if\nvoid real() {}\n", 1));
    CHECK_FALSE(Excludes("#pragma once\nvoid real() {}\n", 1));
}

TEST_CASE("Preprocessor - No directives means nothing is excluded")
{
    CHECK(FindExcludedLineRanges("void main() {}\n").empty());
    CHECK(FindExcludedLineRanges("").empty());
}
