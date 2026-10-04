#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
namespace fs = std::filesystem;

/**
 * @brief Resolves repository root path.
 * @return Path to repository root.
 */
fs::path GetRepoRoot()
{
#ifdef ANGELSCRIPT_REPO_ROOT
    return fs::path(ANGELSCRIPT_REPO_ROOT);
#else
    return fs::current_path();
#endif
}

/**
 * @brief Resolves a stub file path across multiple plausible directories.
 * @param[in] relativePath Relative path from repo root or server.
 * @return Located absolute path, or empty path if not found.
 */
fs::path ResolveStubPath(const std::string& relativePath)
{
    const fs::path root = GetRepoRoot();
    const fs::path candidates[] = {root / relativePath, root / "server" / relativePath,
                                   fs::current_path() / relativePath, fs::current_path() / ".." / relativePath};
    for (const auto& c : candidates)
    {
        if (fs::exists(c))
        {
            return c;
        }
    }
    return {};
}

/**
 * @brief Reads and sanitizes a predefined stub file.
 * @param[in] path File path to read.
 * @return Sanitized stub source code string.
 */
std::string ReadStub(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return angel_lsp::utils::SanitizePredefinedContent(ss.str());
}

/**
 * @brief Loads and indexes a predefined stub into the provided symbol table.
 * @param[in] stubPath Path to predefined stub file.
 * @param[out] table Target symbol table to populate.
 * @param[out] elapsedMs Measured load and parse elapsed milliseconds.
 * @return True if stub was loaded and parsed without syntax errors.
 */
bool LoadPredefinedStub(const fs::path& stubPath, SymbolTable& table, double& elapsedMs)
{
    const std::string content = ReadStub(stubPath);
    if (content.empty())
    {
        return false;
    }

    angel_lsp::utils::HighResTimer timer;
    AngelScriptParser parser;
    angel_lsp::i18n::I18n i18n;
    SymbolCollector collector(nullptr);

    const std::string uri = "file:///" + stubPath.filename().generic_string();
    const auto diagnostics = collector.CollectSymbols({uri, content, &i18n}, parser, table);
    elapsedMs = timer.ElapsedMs();

    return diagnostics.empty();
}

/**
 * @brief Validates a script against a preloaded stub environment.
 * @param[in] script Source code of the script.
 * @param[in] table Preloaded symbol table containing stub symbols.
 * @return List of error-level diagnostics emitted.
 */
std::vector<Diagnostic> AnalyzeScript(const std::string& script, SymbolTable& table)
{
    AngelScriptParser parser;
    angel_lsp::i18n::I18n i18n;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);

    const std::string scriptUri = "file:///test_script.as";
    collector.CollectSymbols({scriptUri, script, &i18n}, parser, table);

    SemanticAnalysisRequest request{table, scriptUri, ".as.predefined", &i18n};
    request.sourceCode = script;
    request.scopeRoot = scopes.CollectScopes(script, parser);

    TSTree* tree = parser.Parse(script);
    request.tree = tree;

    SemanticAnalyzer analyzer(nullptr);
    const auto allDiags = analyzer.Analyze(request);

    if (tree)
    {
        ts_tree_delete(tree);
    }

    std::vector<Diagnostic> errors;
    for (const auto& d : allDiags)
    {
        if (d.severity == DiagnosticSeverity::Error)
        {
            errors.push_back(d);
        }
    }
    return errors;
}

} // namespace

TEST_CASE("External Predefined Stubs - Trackmania Nations Forever (Sashi0034)")
{
    const fs::path stubPath = ResolveStubPath("tests/fixtures/trackmania.as.predefined");
    REQUIRE_MESSAGE(fs::exists(stubPath), "Trackmania predefined stub fixture not found");

    SymbolTable table;
    double elapsedMs = 0.0;
    const bool success = LoadPredefinedStub(stubPath, table, elapsedMs);

    REQUIRE(success);
    CHECK(elapsedMs < 1000.0);

    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("OnTick");
    const std::string varName = angel_lsp::test::GenerateRandomSymbolName("cam");
    const std::string script = "void " + fnName + "() {\n" + "    float val = Math::Clamp(1.5f, 0.0f, 1.0f);\n" +
                               "    TM::GameCamera " + varName + ";\n" + "    vec3 speed = " + varName +
                               ".get_Speed();\n" + "}\n";

    const auto errors = AnalyzeScript(script, table);
    CHECK(errors.empty());
}

TEST_CASE("External Predefined Stubs - OpenSiv3D (Sashi0034)")
{
    const fs::path stubPath = ResolveStubPath("tests/fixtures/opensiv3d.as.predefined");
    REQUIRE_MESSAGE(fs::exists(stubPath), "OpenSiv3D predefined stub fixture not found");

    SymbolTable table;
    double elapsedMs = 0.0;
    const bool success = LoadPredefinedStub(stubPath, table, elapsedMs);

    REQUIRE(success);
    CHECK(elapsedMs < 2000.0);

    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("MainApp");
    const std::string varVec = angel_lsp::test::GenerateRandomSymbolName("position");
    const std::string script = "void " + fnName + "() {\n" + "    Vec2 " + varVec + "(100.0, 200.0);\n" +
                               "    ColorF col = Palette::White;\n" + "}\n";

    const auto errors = AnalyzeScript(script, table);
    CHECK(errors.empty());
}

TEST_CASE("External Predefined Stubs - Sven Co-op Legacy (Sashi0034)")
{
    const fs::path stubPath = ResolveStubPath("tests/fixtures/sven-sashi.as.predefined");
    REQUIRE_MESSAGE(fs::exists(stubPath), "Sven Co-op legacy predefined stub fixture not found");

    SymbolTable table;
    double elapsedMs = 0.0;
    const bool success = LoadPredefinedStub(stubPath, table, elapsedMs);

    REQUIRE(success);
    CHECK(elapsedMs < 5000.0);

    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("MapInit");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string script = "void " + fnName + "(CBasePlayer@ " + paramName + ") {\n" + "    if (" + paramName +
                               " !is null) {\n" + "        Vector origin = " + paramName + ".GetOrigin();\n" +
                               "    }\n" + "}\n";

    const auto errors = AnalyzeScript(script, table);
    CHECK(errors.empty());
}

TEST_CASE("External Predefined Stubs - Sven Co-op Modern (Gaftherman)")
{
    const fs::path stubPath = ResolveStubPath("predefined/sven.as.predefined");
    REQUIRE_MESSAGE(fs::exists(stubPath), "Gaftherman Sven Co-op predefined stub not found");

    SymbolTable table;
    double elapsedMs = 0.0;
    const bool success = LoadPredefinedStub(stubPath, table, elapsedMs);

    REQUIRE(success);
    CHECK(elapsedMs < 5000.0);

    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("PlayerSpawn");
    const std::string paramName = angel_lsp::test::GenerateRandomSymbolName("player");
    const std::string script = "void " + fnName + "(CBasePlayer@ " + paramName + ") {\n" + "    if (" + paramName +
                               " !is null) {\n" + "        g_PlayerFuncs.ClientPrint(" + paramName +
                               ", HUD_PRINTCONSOLE, \"Spawned\\n\");\n" + "    }\n" + "}\n";

    const auto errors = AnalyzeScript(script, table);
    CHECK(errors.empty());
}
