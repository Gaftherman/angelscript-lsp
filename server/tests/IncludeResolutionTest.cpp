#include <doctest/doctest.h>

#include "utils/IncludeResolver.h"

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
