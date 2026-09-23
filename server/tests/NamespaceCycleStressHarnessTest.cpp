#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "utils/MultiFileLogger.h"
#include "utils/Timer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp::test
{
namespace
{
struct StressTestEnvironment
{
    parser::AngelScriptParser parser;
    analysis::SymbolCollector symbolCollector{nullptr};
    analysis::LocalScopeCollector scopeCollector{nullptr};
    analysis::SymbolTable symbolTable;
    analysis::ScopeIndex scopeIndex;
    std::string uri = "file:///stress_test.as";
    std::string sourceCode;
    TSTree* tree = nullptr;

    explicit StressTestEnvironment(const std::string& code)
        : sourceCode(code)
    {
        tree = parser.Parse(sourceCode);
        symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(uri, std::move(rootScope));
        }
    }

    ~StressTestEnvironment()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    std::optional<lsp::Hover> HoverAt(uint32_t line, uint32_t character)
    {
        features::HoverRequest req{uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{line, character}};
        return features::GetHover(req);
    }
};

std::pair<uint32_t, uint32_t> FindPositionOf(const std::string& code, const std::string& token)
{
    const size_t offset = code.find(token);
    if (offset == std::string::npos)
    {
        return {0, 0};
    }
    uint32_t line = 0;
    uint32_t col = 0;
    for (size_t i = 0; i < offset; ++i)
    {
        if (code[i] == '\n')
        {
            line++;
            col = 0;
        }
        else
        {
            col++;
        }
    }
    return {line, col};
}
} // namespace

TEST_CASE("Stress Harness - 20-level nested namespaces upward fallback lookup")
{
    std::vector<std::string> nsNames;
    nsNames.reserve(20);
    for (int i = 0; i < 20; ++i)
    {
        nsNames.push_back(GenerateRandomSymbolName("Ns" + std::to_string(i)));
    }
    const std::string missingSym = GenerateRandomSymbolName("MissingSym");
    const std::string testFunc = GenerateRandomSymbolName("DeepFunc");

    std::string code;
    for (const auto& name : nsNames)
    {
        code += "namespace " + name + " {\n";
    }
    code += "void " + testFunc + "() {\n";
    code += "    " + missingSym + "();\n";
    code += "}\n";
    for (size_t i = 0; i < nsNames.size(); ++i)
    {
        code += "}\n";
    }

    StressTestEnvironment env(code);
    auto [line, col] = FindPositionOf(code, missingSym);

    utils::HighResTimer timer;
    auto hover = env.HoverAt(line, col);
    const double elapsedMs = timer.ElapsedMs();

    // Verify upward fallback returned cleanly without hanging, stack overflow, or OOM
    CHECK_FALSE(hover.has_value());
    CHECK(elapsedMs < 100.0);

    // Directly assert cycle-guarded traversal on corrupted scope structures
    auto rootScope = std::make_unique<analysis::Scope>();
    auto childScope = std::make_unique<analysis::Scope>();
    childScope->parent = rootScope.get();

    // Artificially inject parent cycle
    rootScope->parent = childScope.get();

    analysis::LocalDefinition dummyDef;
    dummyDef.name = "dummy";
    const auto* found = analysis::FindScopeDeclaringDefinition(rootScope.get(), dummyDef);
    CHECK(found == nullptr);

    // Break cycle prior to cleanup
    rootScope->parent = nullptr;
}

TEST_CASE("Stress Harness - Hook hover with anonymous function callback")
{
    const std::string hookInst = GenerateRandomSymbolName("g_Hooks");
    const std::string regFunc = GenerateRandomSymbolName("RegisterHook");
    const std::string cbType = GenerateRandomSymbolName("MapChangeHook");
    const std::string nsHooks = GenerateRandomSymbolName("Hooks");
    const std::string nsGame = GenerateRandomSymbolName("Game");
    const std::string constHook = GenerateRandomSymbolName("MapChange");
    const std::string setupFunc = GenerateRandomSymbolName("SetupHooks");
    const std::string localParam = GenerateRandomSymbolName("mapName");
    const std::string innerVar = GenerateRandomSymbolName("innerCounter");

    std::string code =
        "funcdef void " + cbType + "(string " + localParam + ");\n"
        "class HookRegistry {\n"
        "    void " + regFunc + "(int id, " + cbType + "@ cb) {}\n"
        "}\n"
        "namespace " + nsHooks + " {\n"
        "    namespace " + nsGame + " {\n"
        "        const int " + constHook + " = 101;\n"
        "    }\n"
        "}\n"
        "HookRegistry " + hookInst + ";\n"
        "void " + setupFunc + "() {\n"
        "    " + hookInst + "." + regFunc + "(" + nsHooks + "::" + nsGame + "::" + constHook + ",\n"
        "        @" + cbType + "(function(string " + localParam + ") {\n"
        "            int " + innerVar + " = 42;\n"
        "        }));\n"
        "}\n";

    StressTestEnvironment env(code);

    // 1. Hover on RegisterHook
    auto [regLine, regCol] = FindPositionOf(code, "." + regFunc);
    auto hoverReg = env.HoverAt(regLine, regCol + 1);
    REQUIRE(hoverReg.has_value());
    auto regContent = std::get<lsp::MarkupContent>(hoverReg->contents);
    CHECK(regContent.value.find(regFunc) != std::string::npos);

    // 2. Hover on Hooks
    auto [hooksLine, hooksCol] = FindPositionOf(code, nsHooks + "::" + nsGame);
    auto hoverHooks = env.HoverAt(hooksLine, hooksCol);
    REQUIRE(hoverHooks.has_value());
    auto hooksContent = std::get<lsp::MarkupContent>(hoverHooks->contents);
    CHECK(hooksContent.value.find(nsHooks) != std::string::npos);

    // 3. Hover on Game
    auto [gameLine, gameCol] = FindPositionOf(code, "::" + nsGame + "::");
    auto hoverGame = env.HoverAt(gameLine, gameCol + 2);
    REQUIRE(hoverGame.has_value());
    auto gameContent = std::get<lsp::MarkupContent>(hoverGame->contents);
    CHECK(gameContent.value.find(nsGame) != std::string::npos);

    // 4. Hover on MapChange
    auto [mapLine, mapCol] = FindPositionOf(code, "::" + constHook);
    auto hoverMap = env.HoverAt(mapLine, mapCol + 2);
    REQUIRE(hoverMap.has_value());
    auto mapContent = std::get<lsp::MarkupContent>(hoverMap->contents);
    CHECK(mapContent.value.find(constHook) != std::string::npos);

    // 5. Hover on innerVar inside anonymous function
    auto [varLine, varCol] = FindPositionOf(code, "int " + innerVar);
    auto hoverVar = env.HoverAt(varLine, varCol + 4);
    REQUIRE(hoverVar.has_value());
    auto varContent = std::get<lsp::MarkupContent>(hoverVar->contents);
    CHECK(varContent.value.find(innerVar) != std::string::npos);
}

TEST_CASE("Stress Harness - Multi-file logger channel isolation and timestamp formatting")
{
    const std::string sandboxName = GenerateRandomSymbolName("logger_test");
    const std::filesystem::path sandboxDir = std::filesystem::temp_directory_path() / sandboxName / ".vscode" / "lsp";

    utils::MultiFileLogger logger;
    logger.Initialize(sandboxDir);

    REQUIRE(logger.IsInitialized());
    const auto activeDir = logger.GetActiveLogDirectory();
    CHECK(std::filesystem::exists(activeDir));

    const std::string testMsg = GenerateRandomSymbolName("msg_content");
    logger.LogMaster(utils::MultiFileLogLevel::Info, "Master initialization: " + testMsg);
    logger.LogHover(utils::MultiFileLogLevel::Info, "Hover profile: " + testMsg, 0.42);
    logger.LogAnalysis(utils::MultiFileLogLevel::Warn, "Analysis warning: " + testMsg, 1.23);
    logger.LogSymbols(utils::MultiFileLogLevel::Error, "Symbols error: " + testMsg);
    logger.LogCrash("Crash dump panic: " + testMsg);

    logger.Flush();

    const auto masterFile = activeDir / "master.log";
    const auto hoverFile = activeDir / "hover.log";
    const auto analysisFile = activeDir / "analysis.log";
    const auto symbolsFile = activeDir / "symbols.log";
    const auto crashFile = activeDir / "crash.log";

    CHECK(std::filesystem::exists(masterFile));
    CHECK(std::filesystem::exists(hoverFile));
    CHECK(std::filesystem::exists(analysisFile));
    CHECK(std::filesystem::exists(symbolsFile));
    CHECK(std::filesystem::exists(crashFile));

    // Master sink contains entries from all channels
    std::ifstream masterStream(masterFile);
    std::string masterContent((std::istreambuf_iterator<char>(masterStream)), std::istreambuf_iterator<char>());
    CHECK(masterContent.find("[MASTER]") != std::string::npos);
    CHECK(masterContent.find("[HOVER]") != std::string::npos);
    CHECK(masterContent.find("[ANALYSIS]") != std::string::npos);
    CHECK(masterContent.find("[SYMBOLS]") != std::string::npos);
    CHECK(masterContent.find("[CRASH]") != std::string::npos);
    CHECK(masterContent.find(testMsg) != std::string::npos);

    // Hover sink contains [HOVER] and formatted duration
    std::ifstream hoverStream(hoverFile);
    std::string hoverContent((std::istreambuf_iterator<char>(hoverStream)), std::istreambuf_iterator<char>());
    CHECK(hoverContent.find("[HOVER]") != std::string::npos);
    CHECK(hoverContent.find("0.42ms") != std::string::npos);

    // Analysis sink contains [ANALYSIS] and [WARN]
    std::ifstream analysisStream(analysisFile);
    std::string analysisContent((std::istreambuf_iterator<char>(analysisStream)), std::istreambuf_iterator<char>());
    CHECK(analysisContent.find("[ANALYSIS]") != std::string::npos);
    CHECK(analysisContent.find("[WARN]") != std::string::npos);

    // Crash sink contains [CRASH] and [FATAL]
    std::ifstream crashStream(crashFile);
    std::string crashContent((std::istreambuf_iterator<char>(crashStream)), std::istreambuf_iterator<char>());
    CHECK(crashContent.find("[CRASH]") != std::string::npos);
    CHECK(crashContent.find("[FATAL]") != std::string::npos);

    // Timestamp formatting: [YYYY-MM-DD HH:mm:ss.mmm]
    const std::regex tsRegex(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\])");
    CHECK(std::regex_search(masterContent, tsRegex));

    // Cleanup
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::temp_directory_path() / sandboxName, ec);
}
} // namespace angel_lsp::test
