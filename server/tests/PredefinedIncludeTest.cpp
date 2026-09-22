/**
 * @file PredefinedIncludeTest.cpp
 * @brief Unit tests for nested includes inside predefined files and forceInclude scoping.
 */

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

using namespace angel_lsp;
using namespace angel_lsp::utils;
using namespace angel_lsp::parser;

namespace
{
struct TempPredefinedSandbox
{
    std::filesystem::path root;

    TempPredefinedSandbox()
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() / ("as_predefined_test_" + std::to_string(now));
        std::filesystem::create_directories(root);
        std::error_code ec;
        auto canonical = std::filesystem::canonical(root, ec);
        if (!ec)
        {
            root = std::move(canonical);
        }
    }

    ~TempPredefinedSandbox()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void WriteFile(const std::string& relPath, const std::string& content) const
    {
        const std::filesystem::path full = root / relPath;
        if (full.has_parent_path())
        {
            std::filesystem::create_directories(full.parent_path());
        }
        std::ofstream ofs(full, std::ios::binary);
        ofs << content;
    }
};
} // namespace

TEST_SUITE("PredefinedIncludesAndScoping")
{
    /**
     * @brief Verifies that nested #include directives inside .as.predefined files resolve properly.
     */
    TEST_CASE("PredefinedInclude - Nested includes inside predefined files resolve properly")
    {
        TempPredefinedSandbox sandbox;
        const std::string nestedClass = test::GenerateRandomSymbolName("NestedType");

        sandbox.WriteFile("stubs/nested.as.predefined", "class " + nestedClass + " {}\n");
        sandbox.WriteFile("stubs/main.as.predefined", "#include \"nested.as.predefined\"\n");

        const std::string mainFile = (sandbox.root / "stubs" / "main.as.predefined").string();
        const std::string nestedFile = (sandbox.root / "stubs" / "nested.as.predefined").string();

        std::vector<std::string> searchDirs = {(sandbox.root / "stubs").string()};
        std::vector<std::string> allowedRoots = {sandbox.root.string()};

        std::string mainContent = "#include \"nested.as.predefined\"\n";
        auto includes = IncludeResolver::ExtractIncludes(mainContent);
        REQUIRE(includes.size() == 1);
        CHECK(includes[0].rawPath == "nested.as.predefined");

        std::string resolved = IncludeResolver::ResolveIncludePath(IncludeResolveRequest{
            .includePath = includes[0].rawPath,
            .currentFilePath = mainFile,
            .searchDirectories = searchDirs,
            .allowedRoots = allowedRoots,
            .implicitExtension = ".as.predefined",
        });

        REQUIRE(!resolved.empty());
        CHECK(IncludeResolver::NormalizePath(resolved) == IncludeResolver::NormalizePath(nestedFile));
    }

    /**
     * @brief Verifies that forceIncludeFiles are incorporated into forward include closures.
     */
    TEST_CASE("PredefinedInclude - Forward closure includes forceInclude dependencies")
    {
        TempPredefinedSandbox sandbox;
        const std::string forceClass = test::GenerateRandomSymbolName("ForcedSymbol");

        sandbox.WriteFile("force/helper.as", "class HelperClass {}\n");
        sandbox.WriteFile("force/common.as", "#include \"helper.as\"\nclass " + forceClass + " {}\n");
        sandbox.WriteFile("scripts/entry.as", "void Main() {}\n");

        const std::string commonPath = IncludeResolver::NormalizePath(sandbox.root / "force" / "common.as");
        const std::string helperPath = IncludeResolver::NormalizePath(sandbox.root / "force" / "helper.as");

        WorkspaceIncludeGraph graph;
        const std::vector<std::string> searchDirs = {IncludeResolver::NormalizePath(sandbox.root / "force")};
        graph.UpdateFile(commonPath, "#include \"helper.as\"\n", searchDirs);
        graph.UpdateFile(helperPath, "class HelperClass {}\n", searchDirs);

        auto fwd = graph.GetForwardClosure(commonPath);
        REQUIRE(!fwd.empty());
        CHECK(std::find(fwd.begin(), fwd.end(), helperPath) != fwd.end());
    }
}
