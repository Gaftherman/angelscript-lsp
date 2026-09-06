#include <doctest/doctest.h>

#include "utils/IncludeResolver.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace angel_lsp::utils;

namespace
{
    /**
     * @brief RAII helper to create and automatically clean up temporary directories for disk-based tests.
     */
    struct TempDirGuard
    {
        std::filesystem::path dir;

        explicit TempDirGuard(const std::string &prefix)
        {
            auto base = std::filesystem::temp_directory_path();
            auto uniqueSuffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            dir = base / (prefix + "_" + uniqueSuffix);
            std::filesystem::create_directories(dir);
        }

        ~TempDirGuard()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void WriteFile(const std::string &relativePath, const std::string &content)
        {
            std::filesystem::path fullPath = dir / relativePath;
            if (fullPath.has_parent_path())
            {
                std::filesystem::create_directories(fullPath.parent_path());
            }
            std::ofstream out(fullPath, std::ios::binary);
            out << content;
            out.close();
        }

        std::string PathString(const std::string &relativePath = "") const
        {
            std::filesystem::path p = relativePath.empty() ? dir : (dir / relativePath);
            std::error_code ec;
            std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
            std::string s = canon.string();
#if defined(_WIN32)
            if (s.rfind("\\\\?\\", 0) == 0)
            {
                s = s.substr(4);
            }
#endif
            std::replace(s.begin(), s.end(), '\\', '/');
            return s;
        }
    };
}

// =====================================================================================
// 1. Directive Extraction Tests
// =====================================================================================

TEST_CASE("IncludeResolver - Extract double-quoted include")
{
    std::string source = "#include \"math/Vector3.as\"";
    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "math/Vector3.as");
    CHECK(includes[0].line == 0);
    CHECK(includes[0].isAngled == false);
}

TEST_CASE("IncludeResolver - Extract angle-bracketed include")
{
    std::string source = "#include <engine/Core.as>";
    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "engine/Core.as");
    CHECK(includes[0].line == 0);
    CHECK(includes[0].isAngled == true);
}

TEST_CASE("IncludeResolver - Extract include with whitespace variations")
{
    std::string source = "   #   include    \t   \"shared/types.as\"   \n\t#\tinclude\t<system.as>\t";
    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 2);
    CHECK(includes[0].rawPath == "shared/types.as");
    CHECK(includes[0].line == 0);
    CHECK(includes[0].isAngled == false);

    CHECK(includes[1].rawPath == "system.as");
    CHECK(includes[1].line == 1);
    CHECK(includes[1].isAngled == true);
}

TEST_CASE("IncludeResolver - Ignore includes in single-line comments")
{
    std::string source =
        "// #include \"ignored1.as\"\n"
        "#include \"valid.as\"\n"
        "   //   #include <ignored2.as>\n";

    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "valid.as");
    CHECK(includes[0].line == 1);
}

TEST_CASE("IncludeResolver - Ignore includes in multi-line block comments")
{
    std::string source =
        "/*\n"
        " #include \"ignored_in_block1.as\"\n"
        "*/\n"
        "#include \"valid.as\"\n"
        "/* single line block */ #include \"after_block.as\"\n"
        "/* #include \"ignored_in_block2.as\" */\n";

    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 2);
    CHECK(includes[0].rawPath == "valid.as");
    CHECK(includes[0].line == 3);
    CHECK(includes[1].rawPath == "after_block.as");
    CHECK(includes[1].line == 4);
}

TEST_CASE("IncludeResolver - Ignore includes in string literals")
{
    std::string source =
        "string s1 = \"#include \\\"ignored1.as\\\"\";\n"
        "#include \"real.as\"\n"
        "string s2 = @\"#include \"\"ignored2.as\"\"\";\n"
        "string s3 = \"\"\" #include \"ignored3.as\" \"\"\";\n"
        "char c = '#';\n";

    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "real.as");
    CHECK(includes[0].line == 1);
    CHECK(includes[0].isAngled == false);
}

TEST_CASE("IncludeResolver - Ignore non-include preprocessor directives")
{
    std::string source =
        "#define FOO 1\n"
        "#ifdef FOO\n"
        "#pragma once\n"
        "#include_next <header.as>\n"
        "#include \"expected.as\"\n"
        "#endif\n";

    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "expected.as");
    CHECK(includes[0].line == 4);
}

TEST_CASE("IncludeResolver - Multi-line file with multiple includes and accurate line numbers")
{
    std::string source =
        "// Header comment\n"
        "#include \"common/Defs.as\"\n"
        "\n"
        "void foo() {}\n"
        "\n"
        "#include <engine/Math.as>\n"
        "/* block comment */\n"
        "\n"
        "#include \"local/Helper.as\"\n";

    auto includes = IncludeResolver::ExtractIncludes(source);

    REQUIRE(includes.size() == 3);
    CHECK(includes[0].rawPath == "common/Defs.as");
    CHECK(includes[0].line == 1);
    CHECK(includes[0].isAngled == false);

    CHECK(includes[1].rawPath == "engine/Math.as");
    CHECK(includes[1].line == 5);
    CHECK(includes[1].isAngled == true);

    CHECK(includes[2].rawPath == "local/Helper.as");
    CHECK(includes[2].line == 8);
    CHECK(includes[2].isAngled == false);
}

// =====================================================================================
// 2. Path Resolution Tests (Relative, Search Directories, Precedence)
// =====================================================================================

TEST_CASE("IncludeResolver - Resolve path relative to current file")
{
    TempDirGuard temp("inc_rel_test");
    temp.WriteFile("src/main.as", "#include \"utils/math.as\"");
    temp.WriteFile("src/utils/math.as", "// math header");

    std::string currentFile = temp.PathString("src/main.as");
    std::string resolved = IncludeResolver::ResolveIncludePath("utils/math.as", currentFile, {});

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/utils/math.as"));
}

TEST_CASE("IncludeResolver - Resolve path relative to current file with parent navigation")
{
    TempDirGuard temp("inc_parent_test");
    temp.WriteFile("src/sub/worker.as", "#include \"../common/types.as\"");
    temp.WriteFile("src/common/types.as", "// types header");

    std::string currentFile = temp.PathString("src/sub/worker.as");
    std::string resolved = IncludeResolver::ResolveIncludePath("../common/types.as", currentFile, {});

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/common/types.as"));
}

TEST_CASE("IncludeResolver - Fallback to search directories when not in current dir")
{
    TempDirGuard temp("inc_search_test");
    temp.WriteFile("app/main.as", "#include \"engine/audio.as\"");
    temp.WriteFile("shared/engine/audio.as", "// audio header");

    std::string currentFile = temp.PathString("app/main.as");
    std::vector<std::string> searchDirs = { temp.PathString("shared") };

    std::string resolved = IncludeResolver::ResolveIncludePath("engine/audio.as", currentFile, searchDirs);

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("shared/engine/audio.as"));
}

TEST_CASE("IncludeResolver - Search directory precedence")
{
    TempDirGuard temp("inc_precedence_test");
    temp.WriteFile("src/main.as", "#include \"config.as\"");
    temp.WriteFile("include_dir1/config.as", "// config 1");
    temp.WriteFile("include_dir2/config.as", "// config 2");

    std::string currentFile = temp.PathString("src/main.as");
    std::vector<std::string> searchDirs = {
        temp.PathString("include_dir1"),
        temp.PathString("include_dir2")
    };

    std::string resolved = IncludeResolver::ResolveIncludePath("config.as", currentFile, searchDirs);
    CHECK(resolved == temp.PathString("include_dir1/config.as"));

    // Reverse order
    std::vector<std::string> reversedDirs = {
        temp.PathString("include_dir2"),
        temp.PathString("include_dir1")
    };
    std::string resolvedReversed = IncludeResolver::ResolveIncludePath("config.as", currentFile, reversedDirs);
    CHECK(resolvedReversed == temp.PathString("include_dir2/config.as"));
}

TEST_CASE("IncludeResolver - Nonexistent include returns empty string")
{
    TempDirGuard temp("inc_nonexist_test");
    temp.WriteFile("src/main.as", "#include \"missing.as\"");

    std::string currentFile = temp.PathString("src/main.as");
    std::vector<std::string> searchDirs = { temp.PathString("shared") };

    std::string resolved = IncludeResolver::ResolveIncludePath("missing.as", currentFile, searchDirs);
    CHECK(resolved.empty());
}

TEST_CASE("IncludeResolver - Empty include path returns empty string")
{
    TempDirGuard temp("inc_empty_test");
    temp.WriteFile("src/main.as", "");

    std::string currentFile = temp.PathString("src/main.as");
    std::string resolved = IncludeResolver::ResolveIncludePath("", currentFile, {});
    CHECK(resolved.empty());
}

// =====================================================================================
// 3. Recursive Resolution, Cycle Detection, and Diamond Patterns
// =====================================================================================

TEST_CASE("IncludeResolver - Linear recursive include resolution on disk")
{
    TempDirGuard temp("inc_linear_test");
    temp.WriteFile("A.as", "#include \"B.as\"");
    temp.WriteFile("B.as", "#include \"C.as\"");
    temp.WriteFile("C.as", "// leaf");

    std::string rootFile = temp.PathString("A.as");
    auto allIncludes = IncludeResolver::ResolveAllIncludes(rootFile, {});

    REQUIRE(allIncludes.size() == 2);
    CHECK(allIncludes[0] == temp.PathString("B.as"));
    CHECK(allIncludes[1] == temp.PathString("C.as"));
}

TEST_CASE("IncludeResolver - Mutual include cycle prevention")
{
    TempDirGuard temp("inc_cycle_test");
    temp.WriteFile("A.as", "#include \"B.as\"");
    temp.WriteFile("B.as", "#include \"A.as\"");

    std::string rootFile = temp.PathString("A.as");
    auto allIncludes = IncludeResolver::ResolveAllIncludes(rootFile, {});

    // A includes B, B includes A (already visited), terminates cleanly
    REQUIRE(allIncludes.size() == 1);
    CHECK(allIncludes[0] == temp.PathString("B.as"));
}

TEST_CASE("IncludeResolver - Self-include cycle prevention")
{
    TempDirGuard temp("inc_self_test");
    temp.WriteFile("Self.as", "#include \"Self.as\"");

    std::string rootFile = temp.PathString("Self.as");
    auto allIncludes = IncludeResolver::ResolveAllIncludes(rootFile, {});

    // Root is visited before scanning, Self.as includes Self.as -> 0 additional files
    CHECK(allIncludes.empty());
}

TEST_CASE("IncludeResolver - Diamond include pattern resolution")
{
    TempDirGuard temp("inc_diamond_test");
    // A -> B, C
    // B -> D
    // C -> D
    temp.WriteFile("A.as", "#include \"B.as\"\n#include \"C.as\"");
    temp.WriteFile("B.as", "#include \"D.as\"");
    temp.WriteFile("C.as", "#include \"D.as\"");
    temp.WriteFile("D.as", "// shared base");

    std::string rootFile = temp.PathString("A.as");
    auto allIncludes = IncludeResolver::ResolveAllIncludes(rootFile, {});

    // B, C, D should all be included exactly once
    REQUIRE(allIncludes.size() == 3);
    CHECK(allIncludes[0] == temp.PathString("B.as"));
    CHECK(allIncludes[1] == temp.PathString("C.as"));
    CHECK(allIncludes[2] == temp.PathString("D.as"));
}

TEST_CASE("IncludeResolver - Custom fileReader callback in ResolveAllIncludes")
{
    TempDirGuard temp("inc_reader_test");
    temp.WriteFile("root.as", "");
    temp.WriteFile("dep1.as", "");
    temp.WriteFile("dep2.as", "");

    std::string rootPath = temp.PathString("root.as");
    std::string dep1Path = temp.PathString("dep1.as");
    std::string dep2Path = temp.PathString("dep2.as");

    std::unordered_map<std::string, std::string> virtualFiles = {
        { rootPath, "#include \"dep1.as\"\n#include \"dep2.as\"" },
        { dep1Path, "// dep1" },
        { dep2Path, "// dep2" }
    };

    auto mockReader = [&](const std::string &path) -> std::string
    {
        auto it = virtualFiles.find(path);
        if (it != virtualFiles.end())
        {
            return it->second;
        }
        return "";
    };

    auto allIncludes = IncludeResolver::ResolveAllIncludes(rootPath, {}, mockReader);

    REQUIRE(allIncludes.size() == 2);
    CHECK(allIncludes[0] == dep1Path);
    CHECK(allIncludes[1] == dep2Path);
}

// =====================================================================================
// Confinement.
//
// `#include` takes whatever text sits between the quotes. Absolute paths were honoured verbatim
// and `../` was unbounded, so a .as file in an untrusted repository could name any file the server
// process could read - and because an included file is parsed, indexed and its text retained, the
// contents came back to the client through hover, definition and references. Opening the repo was
// the whole exploit.
//
// Empty roots still mean unconfined: that is what a library caller or a unit test with no
// workspace context gets, and it is why the cases above still resolve `../common/types.as`.
// =====================================================================================

TEST_CASE("IncludeResolver - Confines resolution to the allowed roots")
{
    TempDirGuard ws("as_confinement");
    ws.WriteFile("workspace/main.as", "#include \"lib/util.as\"\n");
    ws.WriteFile("workspace/lib/util.as", "void util() {}\n");
    ws.WriteFile("secrets/private.as", "void secret() {}\n");

    const std::string workspaceRoot = ws.PathString("workspace");
    const std::string currentFile = ws.PathString("workspace/main.as");
    const std::vector<std::string> roots{ workspaceRoot };

    SUBCASE("A file inside the root still resolves")
    {
        CHECK_FALSE(IncludeResolver::ResolveIncludePath("lib/util.as", currentFile, {}, roots).empty());
    }

    SUBCASE("A relative walk out of the workspace resolves to nothing")
    {
        // Resolves fine unconfined - proving the file is really there and the check is what stops it.
        CHECK_FALSE(IncludeResolver::ResolveIncludePath("../secrets/private.as", currentFile, {}).empty());
        CHECK(IncludeResolver::ResolveIncludePath("../secrets/private.as", currentFile, {}, roots).empty());
    }

    SUBCASE("An absolute path outside the workspace resolves to nothing")
    {
        const std::string absolute = ws.PathString("secrets/private.as");
        CHECK_FALSE(IncludeResolver::ResolveIncludePath(absolute, currentFile, {}).empty());
        CHECK(IncludeResolver::ResolveIncludePath(absolute, currentFile, {}, roots).empty());
    }

    SUBCASE("A search directory is a root in its own right")
    {
        const std::string searchDir = ws.PathString("secrets");
        const std::vector<std::string> withSearchDir{ workspaceRoot, searchDir };

        // Reachable once the operator has explicitly configured that directory, and only then.
        CHECK_FALSE(IncludeResolver::ResolveIncludePath("private.as", currentFile, { searchDir }, withSearchDir).empty());
        CHECK(IncludeResolver::ResolveIncludePath("private.as", currentFile, { searchDir }, roots).empty());
    }

    SUBCASE("A sibling whose name merely starts with the root is not inside it")
    {
        // "workspace" must not be treated as containing "workspace_other" - a prefix compare that
        // ignores path components would let the whole sibling tree through.
        ws.WriteFile("workspace_other/leak.as", "void leak() {}\n");
        const std::string sibling = ws.PathString("workspace_other/leak.as");

        CHECK_FALSE(IncludeResolver::ResolveIncludePath(sibling, currentFile, {}).empty());
        CHECK(IncludeResolver::ResolveIncludePath(sibling, currentFile, {}, roots).empty());
    }
}

TEST_CASE("IncludeResolver - Transitive resolution is confined too")
{
    TempDirGuard ws("as_confinement");
    ws.WriteFile("workspace/root.as", "#include \"mid.as\"\n");
    ws.WriteFile("workspace/mid.as", "#include \"../secrets/private.as\"\n");
    ws.WriteFile("secrets/private.as", "void secret() {}\n");

    const std::vector<std::string> roots{ ws.PathString("workspace") };

    // Without roots the walk escapes at the second hop; with them it stops at mid.as.
    const auto unconfined = IncludeResolver::ResolveAllIncludes(ws.PathString("workspace/root.as"), {}, nullptr);
    CHECK(unconfined.size() == 2);

    const auto confined = IncludeResolver::ResolveAllIncludes(ws.PathString("workspace/root.as"), {}, nullptr, roots);
    REQUIRE(confined.size() == 1);
    CHECK(confined[0].find("mid.as") != std::string::npos);
}

// =====================================================================================
// 4. Implicit Extension Resolution
// =====================================================================================

TEST_CASE("IncludeResolver - Resolves literal extensionless file when no implicit extension is configured")
{
    // Default behavior matches vanilla AngelScript: the path between quotes is opened verbatim, finding a literal extensionless file.
    TempDirGuard temp("inc_implicit_none_exact");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper", "// literal helper\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, {});

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/helper"));
}

TEST_CASE("IncludeResolver - Extensionless include finds nothing when only .as exists and implicit extension is omitted")
{
    // Compiler-exact control: CScriptBuilder never guesses extensions, so an unadorned include must fail when only helper.as exists unless the caller explicitly opted into implicit extension.
    TempDirGuard temp("inc_implicit_none_missing");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper.as", "// helper script\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, {});

    CHECK(resolved.empty());
}

TEST_CASE("IncludeResolver - Resolves extensionless include to .as file when implicit extension is set")
{
    // Host environments like Sven Co-op mandate omitting the extension; configuring an implicit extension retries name + suffix when the exact name is absent.
    TempDirGuard temp("inc_implicit_ext_found");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper.as", "// helper script\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, {}, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/helper.as"));
}

TEST_CASE("IncludeResolver - Include already containing extension resolves on exact pass without appending duplicate extension")
{
    // The exact name is tried first in each directory, so an include already bearing .as matches immediately and never constructs helper.as.as.
    TempDirGuard temp("inc_implicit_ext_already_has_ext");
    temp.WriteFile("src/main.as", "#include \"helper.as\"\n");
    temp.WriteFile("src/helper.as", "// helper script\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper.as", currentFile, {}, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/helper.as"));
}

TEST_CASE("IncludeResolver - Exact match takes precedence over implicit extension when both exist in the same directory")
{
    // Exact lookup must run before suffix retry; otherwise a directory holding both helper and helper.as would silently resolve to the wrong file.
    TempDirGuard temp("inc_implicit_ext_precedence");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper", "// literal helper\n");
    temp.WriteFile("src/helper.as", "// helper script\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, {}, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/helper"));
}

TEST_CASE("IncludeResolver - Retries implicit extension in first directory before inspecting search directories")
{
    // Resolution evaluates candidate locations directory-by-directory: the current file's directory is tried exhaustively (exact, then extended) before any search directory is consulted.
    TempDirGuard temp("inc_implicit_ext_per_dir");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper.as", "// extended helper in current dir\n");
    temp.WriteFile("search/helper", "// exact helper in search dir\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::vector<std::string> searchDirs = { temp.PathString("search") };
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, searchDirs, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/helper.as"));
}

TEST_CASE("IncludeResolver - Subdirectory include path resolves with implicit extension")
{
    // Implicit extension retry applies to paths containing nested subdirectories, appending the suffix to the full relative path.
    TempDirGuard temp("inc_implicit_ext_subdir");
    temp.WriteFile("src/main.as", "#include \"weapons/rifle\"\n");
    temp.WriteFile("src/weapons/rifle.as", "// rifle script\n");

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("weapons/rifle", currentFile, {}, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("src/weapons/rifle.as"));
}

TEST_CASE("IncludeResolver - Parent-relative include resolves with implicit extension as produced by completion")
{
    // Completion items produce parent-relative paths when referencing sibling directories, which must resolve cleanly under implicit extension retry.
    TempDirGuard temp("inc_implicit_ext_parent");
    temp.WriteFile("sub/main.as", "#include \"../shared\"\n");
    temp.WriteFile("shared.as", "// shared root script\n");

    const std::string currentFile = temp.PathString("sub/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("../shared", currentFile, {}, {}, ".as");

    CHECK_FALSE(resolved.empty());
    CHECK(resolved == temp.PathString("shared.as"));
}

TEST_CASE("IncludeResolver - Implicit extension does not defeat allowed roots confinement")
{
    // Confinement checking runs on the resolved path at exit, preventing suffix fallback from escaping allowed workspace roots.
    TempDirGuard ws("inc_implicit_ext_confinement");
    ws.WriteFile("workspace/main.as", "#include \"../secrets/private\"\n");
    ws.WriteFile("secrets/private.as", "// secret\n");

    const std::string workspaceRoot = ws.PathString("workspace");
    const std::string currentFile = ws.PathString("workspace/main.as");
    const std::vector<std::string> roots{ workspaceRoot };

    // Resolves when unconfined, proving the file is present on disk and matches with the implicit extension.
    CHECK_FALSE(IncludeResolver::ResolveIncludePath("../secrets/private", currentFile, {}, {}, ".as").empty());

    // Confined resolution must reject the target because it escapes the allowed roots.
    CHECK(IncludeResolver::ResolveIncludePath("../secrets/private", currentFile, {}, roots, ".as").empty());
}

TEST_CASE("IncludeResolver - Resolves differently-cased include name based on filesystem case sensitivity")
{
    // Whether differently-cased paths refer to the same file is determined by the underlying filesystem rather than the OS, requiring runtime detection rather than compile-time platform checks.
    TempDirGuard temp("inc_implicit_ext_casing");
    temp.WriteFile("src/main.as", "#include \"HELPER\"\n");
    temp.WriteFile("src/helper.as", "// helper script\n");

    std::string shouted = temp.PathString("src/helper.as");
    for (char &c : shouted)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    std::error_code ec;
    const bool caseInsensitive = std::filesystem::exists(std::filesystem::path(shouted), ec) && !ec;

    INFO("case-insensitive filesystem: " << caseInsensitive);

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("HELPER", currentFile, {}, {}, ".as");

    if (caseInsensitive)
    {
        CHECK_FALSE(resolved.empty());
        CHECK(resolved == temp.PathString("src/helper.as"));
    }
    else
    {
        CHECK(resolved.empty());
    }
}

TEST_CASE("IncludeResolver - Resolves uppercase file extension with lowercase implicit extension based on filesystem case sensitivity")
{
    // Matching an uppercase on-disk extension against a lowercase implicit extension succeeds on case-insensitive filesystems and fails on case-sensitive ones, both being correct.
    TempDirGuard temp("inc_implicit_ext_upper_ext");
    temp.WriteFile("src/main.as", "#include \"helper\"\n");
    temp.WriteFile("src/helper.AS", "// helper script with uppercase extension\n");

    std::string lowered = temp.PathString("src/helper.AS");
    for (char &c : lowered)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    std::error_code ec;
    const bool caseInsensitive = std::filesystem::exists(std::filesystem::path(lowered), ec) && !ec;

    INFO("case-insensitive filesystem: " << caseInsensitive);

    const std::string currentFile = temp.PathString("src/main.as");
    const std::string resolved = IncludeResolver::ResolveIncludePath("helper", currentFile, {}, {}, ".as");

    if (caseInsensitive)
    {
        CHECK_FALSE(resolved.empty());
        CHECK(resolved == temp.PathString("src/helper.AS"));
    }
    else
    {
        CHECK(resolved.empty());
    }
}

