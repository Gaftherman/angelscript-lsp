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

// =====================================================================================
// `#define` extraction from predefined stubs.
// =====================================================================================

TEST_CASE("Preprocessor - ScanDefinedWords extracts define directives from stubs")
{
    // A single define directive extracts its identifier.
    CHECK(ScanDefinedWords("#define FOO\n") == std::vector<std::string>{ "FOO" });

    // Multiple directives are returned in their order of appearance.
    CHECK(ScanDefinedWords("#define FIRST\n#define SECOND\n#define THIRD\n") ==
          std::vector<std::string>{ "FIRST", "SECOND", "THIRD" });

    // Leading indentation and spaces between hash and directive name are accepted.
    CHECK(ScanDefinedWords("  \t # \t define   INDENTED\n") == std::vector<std::string>{ "INDENTED" });

    // Directives within single-line comments are ignored.
    CHECK(ScanDefinedWords("// #define IN_LINE_COMMENT\n").empty());

    // Directives within block comments are ignored.
    CHECK(ScanDefinedWords("/*\n#define IN_BLOCK_COMMENT\n*/\n").empty());

    // Directives within string literals are ignored.
    CHECK(ScanDefinedWords("string s = \"#define IN_STRING\";\n").empty());

    // A define directive without an identifier yields nothing.
    CHECK(ScanDefinedWords("#define\n#define   \n").empty());

    // Duplicate defines are preserved in appearance order so caller set insertion is explicit.
    CHECK(ScanDefinedWords("#define DUP\n#define DUP\n") == std::vector<std::string>{ "DUP", "DUP" });

    // A document containing no directives returns an empty vector.
    CHECK(ScanDefinedWords("class Foo { void Bar(); }\n").empty());
    CHECK(ScanDefinedWords("").empty());

    // Directives with names other than define are ignored.
    CHECK(ScanDefinedWords("#defineX FOO\n").empty());
}

TEST_CASE("Preprocessor - Defined words from stub control exclusion in FindExcludedLineRanges")
{
    const std::string stubWithFoo = "#define FOO\n";
    const std::string stubWithoutFoo = "// No defines here\n";

    const std::string source =
        "#if FOO\n"
        "void inside() {}\n"
        "#endif\n";

    const auto wordsFrom = [](std::string_view stub)
    {
        const auto words = ScanDefinedWords(stub);
        return ankerl::unordered_dense::set<std::string>(words.begin(), words.end());
    };

    // When the stub defines FOO, the #if FOO block is preserved and no range is excluded.
    CHECK(FindExcludedLineRanges(source, wordsFrom(stubWithFoo)).empty());

    // When the stub does not define FOO, the whole #if FOO block is excluded.
    const auto excluded = FindExcludedLineRanges(source, wordsFrom(stubWithoutFoo));
    REQUIRE_FALSE(excluded.empty());
    CHECK(IsLineExcluded(excluded, 1));
}


// =====================================================================================
// Host preprocessor extensions.
//
// None of these exist in the stock add-on, and each default was measured against the real compiler
// rather than assumed - see the table in utils/PreprocessorRegions.h. The first test here is the
// one that matters most: it pins the *default*, which has to keep reproducing the stock behaviour
// exactly, because every one of these switches is a promise about a file the host has patched and
// a host that patched nothing must see no change.
// =====================================================================================

namespace
{
    using angel_lsp::utils::PreprocessorFeatures;

    ankerl::unordered_dense::set<std::string> Defined(std::initializer_list<std::string> words)
    {
        return ankerl::unordered_dense::set<std::string>(words);
    }
}

TEST_CASE("Preprocessor - By default #else is swallowed by the block it sits in")
{
    // Measured, not assumed. Running this through the real compiler, `main` calling into the
    // `#else` branch fails with "No matching symbol", not with a complaint about `#else`: the whole
    // block went, `#else` included, because `#else` is not a directive to the stock add-on and the
    // exclusion runs to the `#endif` regardless of what is in between.
    const std::string source =
        "#if FOO\n"       // 0
        "void a() {}\n"   // 1
        "#else\n"         // 2
        "void b() {}\n"   // 3
        "#endif\n";       // 4

    const auto ranges = FindExcludedLineRanges(source, {});

    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].startLine == 0);
    CHECK(ranges[0].endLine == 4);

    // Including the branch a reader would expect to survive.
    CHECK(IsLineExcluded(ranges, 3));
}

TEST_CASE("Preprocessor - With elseSupport the two branches are separate")
{
    const std::string source =
        "#if FOO\n"       // 0
        "void a() {}\n"   // 1
        "#else\n"         // 2
        "void b() {}\n"   // 3
        "#endif\n";       // 4

    PreprocessorFeatures features;
    features.elseSupport = true;

    SUBCASE("the word is undefined, so the first branch goes and the second stays")
    {
        const auto ranges = FindExcludedLineRanges(source, {}, features);

        CHECK(IsLineExcluded(ranges, 1));
        CHECK_FALSE(IsLineExcluded(ranges, 3));
    }

    SUBCASE("the word is defined, so it is the other way round")
    {
        const auto ranges = FindExcludedLineRanges(source, Defined({ "FOO" }), features);

        CHECK_FALSE(IsLineExcluded(ranges, 1));
        CHECK(IsLineExcluded(ranges, 3));
    }
}

TEST_CASE("Preprocessor - With elifSupport the first true branch is the only live one")
{
    const std::string source =
        "#if FOO\n"       // 0
        "void a() {}\n"   // 1
        "#elif BAR\n"     // 2
        "void b() {}\n"   // 3
        "#elif BAZ\n"     // 4
        "void c() {}\n"   // 5
        "#else\n"         // 6
        "void d() {}\n"   // 7
        "#endif\n";       // 8

    PreprocessorFeatures features;
    features.elseSupport = true;
    features.elifSupport = true;

    SUBCASE("a later branch is dead once an earlier one was taken")
    {
        // Both BAR and BAZ are defined; only BAR's branch is live, and that is the whole rule.
        const auto ranges = FindExcludedLineRanges(source, Defined({ "BAR", "BAZ" }), features);

        CHECK(IsLineExcluded(ranges, 1));
        CHECK_FALSE(IsLineExcluded(ranges, 3));
        CHECK(IsLineExcluded(ranges, 5));
        CHECK(IsLineExcluded(ranges, 7));
    }

    SUBCASE("with nothing defined the #else branch is the one that survives")
    {
        const auto ranges = FindExcludedLineRanges(source, {}, features);

        CHECK(IsLineExcluded(ranges, 1));
        CHECK(IsLineExcluded(ranges, 3));
        CHECK(IsLineExcluded(ranges, 5));
        CHECK_FALSE(IsLineExcluded(ranges, 7));
    }
}

TEST_CASE("Preprocessor - ifdefSupport opens a region, and ifndef negates it")
{
    const std::string source =
        "#ifdef FOO\n"    // 0
        "void a() {}\n"   // 1
        "#endif\n"        // 2
        "#ifndef FOO\n"   // 3
        "void b() {}\n"   // 4
        "#endif\n";       // 5

    PreprocessorFeatures features;
    features.ifdefSupport = true;

    const auto ranges = FindExcludedLineRanges(source, Defined({ "FOO" }), features);

    CHECK_FALSE(IsLineExcluded(ranges, 1));
    CHECK(IsLineExcluded(ranges, 4));
}

TEST_CASE("Preprocessor - Without ifdefSupport neither one opens anything")
{
    // The stock add-on leaves `#ifdef` in the source and the compiler reports an unexpected token,
    // so it excludes nothing at all - not even the body a reader would expect it to guard.
    const std::string source =
        "#ifdef FOO\n"
        "void a() {}\n"
        "#endif\n";

    CHECK(FindExcludedLineRanges(source, {}).empty());
}

TEST_CASE("Preprocessor - defineInScripts defines from its own line onwards")
{
    const std::string source =
        "#if FOO\n"       // 0
        "void early() {}\n"
        "#endif\n"        // 2
        "#define FOO\n"   // 3
        "#if FOO\n"       // 4
        "void late() {}\n"// 5
        "#endif\n";       // 6

    PreprocessorFeatures features;
    features.defineInScripts = true;

    const auto ranges = FindExcludedLineRanges(source, {}, features);

    // The first block is still excluded: a `#define` cannot reach backwards.
    CHECK(IsLineExcluded(ranges, 1));
    CHECK_FALSE(IsLineExcluded(ranges, 5));
}

TEST_CASE("Preprocessor - Without defineInScripts a #define in a script defines nothing")
{
    const std::string source =
        "#define FOO\n"
        "#if FOO\n"
        "void late() {}\n"
        "#endif\n";

    // Which is right: the stock add-on does not recognise `#define`, leaves it in the source, and
    // the compiler rejects the file. Silence about the block is the safe half of that.
    CHECK(IsLineExcluded(FindExcludedLineRanges(source, {}), 2));
}

TEST_CASE("Preprocessor - A branch boundary inside a nested dead block belongs to that block")
{
    // The inner `#else` must not be mistaken for the outer one's, or the outer live branch would
    // start in the middle of a block that is going away.
    const std::string source =
        "#if OUTER\n"      // 0
        "#if INNER\n"      // 1
        "void a() {}\n"    // 2
        "#else\n"          // 3
        "void b() {}\n"    // 4
        "#endif\n"         // 5
        "#else\n"          // 6
        "void c() {}\n"    // 7
        "#endif\n";        // 8

    PreprocessorFeatures features;
    features.elseSupport = true;

    const auto ranges = FindExcludedLineRanges(source, {}, features);

    // OUTER is undefined, so everything up to its `#else` goes - both inner branches with it - and
    // only the outer `#else` branch survives.
    CHECK(IsLineExcluded(ranges, 2));
    CHECK(IsLineExcluded(ranges, 4));
    CHECK_FALSE(IsLineExcluded(ranges, 7));
}
