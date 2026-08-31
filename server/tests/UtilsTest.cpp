#include <doctest/doctest.h>

#include "utils/Utils.h"

using namespace angel_lsp::utils;

// =====================================================================================
// IsPredefinedFile - the real matching logic Server::ReadWorkspaceFiles uses to decide
// whether a workspace file gets loaded as an engine-registration stub (see
// SemanticAnalyzerTest.cpp's "predefined-style file" test for what happens once one is).
// =====================================================================================

TEST_CASE("IsPredefinedFile - file ending with the configured extension matches")
{
    CHECK(IsPredefinedFile("file:///project/stubs.as.predefined", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - an ordinary script file does not match")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/script.as", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - the extension must be a true suffix, not just a substring")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/foo.as.predefined.txt", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - an empty extension never matches")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/stubs.as.predefined", ""));
}

TEST_CASE("IsPredefinedFile - the conventional 'as.predefined' filename matches")
{
    // AngelScript's own convention, and what the community's stubs are called. It is not a suffix
    // match: the default `.as.predefined` wants a dot where such a path has a separator, so the
    // 646 KB Sven Coop stub was never picked up by a workspace scan and every host type stayed
    // invisible. The extension already registers this filename as AngelScript in package.json.
    CHECK(IsPredefinedFile("file:///c%3A/Users/Fano/Documents/LSP/as.predefined", ".as.predefined"));
    CHECK(IsPredefinedFile("C:\\Users\\Fano\\Documents\\LSP\\as.predefined", ".as.predefined"));
    CHECK(IsPredefinedFile("as.predefined", ".as.predefined"));

    // Still recognised when no suffix is configured at all - it stands on its own name.
    CHECK(IsPredefinedFile("file:///project/as.predefined", ""));
}

TEST_CASE("IsPredefinedFile - a file merely ending in 'as.predefined' does not match")
{
    // `overloads.predefined` is not the conventional name, and neither is anything that only ends
    // with those characters. The name has to start at a path separator.
    CHECK_FALSE(IsPredefinedFile("file:///project/nonsenseas.predefined", ""));
    CHECK_FALSE(IsPredefinedFile("file:///project/my-as.predefined", ""));
}

TEST_CASE("IsPredefinedFile - the configured suffix keeps working alongside it")
{
    CHECK(IsPredefinedFile("file:///project/engine.as.predefined", ".as.predefined"));
    CHECK_FALSE(IsPredefinedFile("file:///project/engine.as.predefined", ".stub"));
}

// =====================================================================================
// IsPrimitiveType - validates AngelScript built-in primitive type names
// =====================================================================================

TEST_CASE("IsPrimitiveType - standard integer types match")
{
    CHECK(IsPrimitiveType("int"));
    CHECK(IsPrimitiveType("int8"));
    CHECK(IsPrimitiveType("int16"));
    CHECK(IsPrimitiveType("int32"));
    CHECK(IsPrimitiveType("int64"));
    CHECK(IsPrimitiveType("uint"));
    CHECK(IsPrimitiveType("uint8"));
    CHECK(IsPrimitiveType("uint16"));
    CHECK(IsPrimitiveType("uint32"));
    CHECK(IsPrimitiveType("uint64"));
}

TEST_CASE("IsPrimitiveType - floating point, boolean, and void types match")
{
    CHECK(IsPrimitiveType("float"));
    CHECK(IsPrimitiveType("double"));
    CHECK(IsPrimitiveType("bool"));
    CHECK(IsPrimitiveType("void"));
}

TEST_CASE("IsPrimitiveType - non-primitive types and user types do not match")
{
    CHECK_FALSE(IsPrimitiveType("string"));
    CHECK_FALSE(IsPrimitiveType("array"));
    CHECK_FALSE(IsPrimitiveType("dictionary"));
    CHECK_FALSE(IsPrimitiveType("Vector3"));
    CHECK_FALSE(IsPrimitiveType("int128"));
    CHECK_FALSE(IsPrimitiveType("uint128"));
    CHECK_FALSE(IsPrimitiveType("Int"));
    CHECK_FALSE(IsPrimitiveType("BOOL"));
    CHECK_FALSE(IsPrimitiveType(""));
    CHECK_FALSE(IsPrimitiveType("int32_t"));
}

// =====================================================================================
// Document - validates Layer 1 Document struct
// =====================================================================================

#include "document/Document.h"

TEST_CASE("Document - struct initialization and fields")
{
    angel_lsp::document::Document doc{"file:///test.as", "void main() {}", 1, nullptr};
    CHECK(doc.uri == "file:///test.as");
    CHECK(doc.text == "void main() {}");
    CHECK(doc.version == 1);
    CHECK(doc.tree == nullptr);
}



// =====================================================================================
// Exclude globs.
//
// Three recursive walks cross every workspace root - the include graph, the predefined-stub scan
// and the engine-profile detector - and none of them could be told to stop. The profile detector
// was the worst: it collected the name of every file it saw, with no extension filter at all.
//
// The syntax is the one every editor's exclude settings already use, so a user can paste what they
// have: `?` for one character, `*` within a segment, `**` across segments.
// =====================================================================================

TEST_CASE("Utils - MatchesGlob handles the three wildcards")
{
    using angel_lsp::utils::MatchesGlob;

    // `**` spans any number of segments, including none.
    CHECK(MatchesGlob("build/CMakeCache.txt", "**/CMakeCache.txt"));
    CHECK(MatchesGlob("CMakeCache.txt", "**/CMakeCache.txt"));
    CHECK(MatchesGlob("a/b/c/build/x/y.as", "**/build/**"));
    CHECK_FALSE(MatchesGlob("a/b/c/rebuild/x/y.as", "**/build/**"));

    // `*` stays inside one segment.
    CHECK(MatchesGlob("src/main.as", "src/*.as"));
    CHECK_FALSE(MatchesGlob("src/deep/main.as", "src/*.as"));
    CHECK(MatchesGlob("src/deep/main.as", "src/**/*.as"));

    // `?` is exactly one character.
    CHECK(MatchesGlob("a1.as", "a?.as"));
    CHECK_FALSE(MatchesGlob("a12.as", "a?.as"));

    // Windows separators read the same as forward slashes.
    CHECK(MatchesGlob(R"(C:\work\build\x.as)", "**/build/**"));
}

TEST_CASE("Utils - A directory is pruned when a pattern reaches into it")
{
    using angel_lsp::utils::IsExcludedDirectory;

    const std::vector<std::string> defaults = { "**/.git/**", "**/build/**", "**/node_modules/**" };

    // The directory itself matches, which is what pruning needs - the pattern names what is INSIDE
    // it, and testing the directory against the pattern unchanged would never fire.
    CHECK(IsExcludedDirectory("C:/work/project/build", defaults));
    CHECK(IsExcludedDirectory("C:/work/project/.git", defaults));
    CHECK(IsExcludedDirectory("C:/work/project/sub/node_modules", defaults));

    // And a source directory is not.
    CHECK_FALSE(IsExcludedDirectory("C:/work/project/src", defaults));
    CHECK_FALSE(IsExcludedDirectory("C:/work/project/scripts/build_tools", defaults));

    // An empty list excludes nothing, which is what every caller that passes no globs relies on.
    CHECK_FALSE(IsExcludedDirectory("C:/work/project/build", {}));
}
