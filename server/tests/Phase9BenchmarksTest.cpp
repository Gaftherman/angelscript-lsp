/**
 * @file Phase9BenchmarksTest.cpp
 * @brief High-scale performance benchmarks for Phase 9 (Ultra-Performance Architecture).
 *
 * Evaluates latency, memory efficiency, and algorithmic invariants across three scenarios:
 * - Scenario A: Massive Predefined File (sven.as.predefined, 1.26 MB, ~14k lines).
 * - Scenario B: Massive Ordinary Script (10k-14k lines with dense logic and expressions).
 * - Scenario C: Large Multi-File Project with Massive #include DAG and background stubs.
 */

#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "features/hover/HoverHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace angel_lsp;
using namespace angel_lsp::utils;

/**
 * @brief RAII sandbox guard for temporary multi-file project directories.
 */
struct BenchmarkSandbox
{
    std::filesystem::path rootDir;

    BenchmarkSandbox()
    {
        const std::string name = test::GenerateRandomSymbolName("perf_sandbox");
        rootDir = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(rootDir);
        std::error_code ec;
        auto canonical = std::filesystem::weakly_canonical(rootDir, ec);
        if (!ec)
        {
            rootDir = std::move(canonical);
        }
    }

    ~BenchmarkSandbox()
    {
        std::error_code ec;
        std::filesystem::remove_all(rootDir, ec);
    }

    std::string WriteFile(const std::string& relativePath, const std::string& content) const
    {
        const auto fullPath = rootDir / relativePath;
        std::filesystem::create_directories(fullPath.parent_path());
        std::ofstream out(fullPath, std::ios::binary);
        out << content;
        return IncludeResolver::NormalizePath(fullPath);
    }
};

/**
 * @brief Reads a text file completely into a string.
 * @param[in] path Filesystem path to read.
 * @return File content string or empty string on failure.
 */
std::string ReadEntireFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * @brief Generates an ordinary massive AngelScript file with realistic methods, loops, and math.
 * @param[in] targetLines Minimum number of code lines to generate.
 * @param[in] rng Random number engine.
 * @return Formatted AngelScript code string.
 */
std::string GenerateMassiveScript(size_t targetLines, std::mt19937_64& rng)
{
    std::string code;
    code.reserve(targetLines * 50);

    code += "// Auto-generated massive script benchmark\n";
    code += "namespace BenchmarkModule\n{\n";

    size_t currentLine = 3;
    size_t classIdx = 0;
    std::uniform_int_distribution<int> coeffDist(2, 9);

    while (currentLine < targetLines)
    {
        const std::string clsName = test::GenerateRandomSymbolName("Cls") + std::to_string(classIdx++);
        code += "    class " + clsName + "\n    {\n";
        code += "        int m_id;\n";
        code += "        float m_score;\n";
        code += "        bool m_active;\n\n";
        currentLine += 6;

        for (int m = 0; m < 10 && currentLine < targetLines; ++m)
        {
            const std::string fnName = test::GenerateRandomSymbolName("Compute") + std::to_string(m);
            const int coeff = coeffDist(rng);
            code += "        int " + fnName + "(int a, float b)\n        {\n";
            code += "            int sum = a + " + std::to_string(coeff) + ";\n";
            code += "            float mult = b * 2.0f;\n";
            code += "            for (int i = 0; i < 5; ++i)\n            {\n";
            code += "                sum += i * 3;\n";
            code += "                if (sum > 100)\n                {\n";
            code += "                    sum -= 50;\n";
            code += "                }\n";
            code += "            }\n";
            code += "            return sum + int(mult);\n";
            code += "        }\n\n";
            currentLine += 14;
        }

        code += "    }\n\n";
        currentLine += 2;
    }

    code += "}\n";
    return code;
}

/**
 * @brief Creates a multi-file project with diamond and chained #include graphs.
 * @param[in] sandbox Sandbox directory guard.
 * @param[in] fileCount Number of files to create.
 * @param[in] rng Random number generator.
 * @return List of normalized file paths.
 */
std::vector<std::string> CreateIncludeProject(const BenchmarkSandbox& sandbox, size_t fileCount, std::mt19937_64& rng)
{
    std::vector<std::string> files;
    files.reserve(fileCount);

    // Shared base file
    std::string sharedContent = "class CoreShared\n{\n    int version;\n    void Init() {}\n}\n";
    files.push_back(sandbox.WriteFile("CoreShared.as", sharedContent));

    for (size_t i = 1; i < fileCount; ++i)
    {
        std::string fileName = "Module_" + std::to_string(i) + ".as";
        std::string content = "#include \"CoreShared.as\"\n";

        // Transitive chain
        if (i > 1)
        {
            content += "#include \"Module_" + std::to_string(i - 1) + ".as\"\n";
        }
        // Diamond inclusion
        if (i >= 4)
        {
            std::uniform_int_distribution<size_t> dist(1, i - 2);
            size_t cross = dist(rng);
            content += "#include \"Module_" + std::to_string(cross) + ".as\"\n";
        }

        content += "class NodeCls_" + std::to_string(i) + "\n{\n";
        content += "    int val_" + std::to_string(i) + ";\n";
        content += "    void Run() {}\n";
        content += "}\n";

        files.push_back(sandbox.WriteFile(fileName, content));
    }

    return files;
}

} // namespace

// =========================================================================================
// Benchmark Suite: Phase 9 Ultra-Performance Tests
// =========================================================================================

TEST_CASE("Benchmark - Scenario A: Massive Predefined File (sven.as.predefined)" * doctest::skip(true))
{
    const std::filesystem::path repoRoot(ANGELSCRIPT_REPO_ROOT);
    const std::filesystem::path predefinedPath = repoRoot / "predefined" / "sven.as.predefined";

    if (!std::filesystem::exists(predefinedPath))
    {
        MESSAGE("Scenario A skipped: predefined/sven.as.predefined not found at: " << predefinedPath.string());
        return;
    }

    const std::string content = ReadEntireFile(predefinedPath);
    REQUIRE(content.size() > 500000);
    const size_t lineCount = static_cast<size_t>(std::count(content.begin(), content.end(), '\n') + 1);

    parser::AngelScriptParser parser;
    HighResTimer parseTimer;
    TSTree* tree = parser.Parse(content);
    const double parseMs = parseTimer.ElapsedMs();
    REQUIRE(tree != nullptr);

    analysis::SymbolTable symbolTable;
    analysis::SymbolCollector collector(nullptr);

    HighResTimer collectTimer;
    collector.CollectSymbolsWithTree(analysis::SymbolCollectRequest("predefined:///sven.as.predefined", content), tree,
                                     symbolTable);
    const double collectMs = collectTimer.ElapsedMs();

    MESSAGE("Scenario A (sven.as.predefined: " << content.size() << " bytes, " << lineCount << " lines):");
    MESSAGE("  - AST Parse: " << parseMs << " ms");
    MESSAGE("  - CollectSymbols: " << collectMs << " ms");
    MESSAGE("  - Symbol Count: " << symbolTable.GetAllSymbols().size());

    CHECK(symbolTable.HasSymbol("Vector"));
    CHECK(symbolTable.HasSymbol("CBasePlayer"));

    analysis::ScopeIndex scopeIndex;
    features::HoverRequest hoverReq{
        "predefined:///sven.as.predefined", content, tree, symbolTable, scopeIndex, lsp::Position{200, 7}
        // "class dictionary" line 201, character 7 (0-indexed line 200)
    };
    HighResTimer hoverTimer;
    auto hoverRes = features::GetHover(hoverReq);
    const double hoverMs = hoverTimer.ElapsedMs();
    MESSAGE("  - GetHover (dictionary): " << hoverMs << " ms");
    CHECK(hoverRes.has_value());

    ts_tree_delete(tree);
}

TEST_CASE("Benchmark - Scenario B: Massive Ordinary Script (10k-14k lines)" * doctest::skip(true))
{
    std::mt19937_64 rng(1337);
    constexpr size_t kTargetLines = 10000;
    const std::string massiveCode = GenerateMassiveScript(kTargetLines, rng);
    const size_t actualLines = static_cast<size_t>(std::count(massiveCode.begin(), massiveCode.end(), '\n') + 1);
    REQUIRE(actualLines >= kTargetLines);

    parser::AngelScriptParser parser;
    HighResTimer parseTimer;
    TSTree* tree = parser.Parse(massiveCode);
    const double parseMs = parseTimer.ElapsedMs();
    REQUIRE(tree != nullptr);

    TSNode rootNode = ts_tree_root_node(tree);

    // 1. NodeIndex::Build
    analysis::NodeIndex nodeIndex;
    HighResTimer indexTimer;
    nodeIndex.Build(rootNode);
    const double indexMs = indexTimer.ElapsedMs();

    // 2. LocalScopeCollector::CollectScopesFromTree
    analysis::LocalScopeCollector scopeCollector(nullptr);
    HighResTimer scopeTimer;
    auto scopeRoot = scopeCollector.CollectScopesFromTree(rootNode, massiveCode);
    const double scopeMs = scopeTimer.ElapsedMs();
    REQUIRE(scopeRoot != nullptr);

    // 3. SymbolCollector
    analysis::SymbolTable symbolTable;
    analysis::SymbolCollector symbolCollector(nullptr);
    HighResTimer symCollectTimer;
    symbolCollector.CollectSymbolsWithTree(analysis::SymbolCollectRequest("file:///massive_benchmark.as", massiveCode),
                                           tree, symbolTable);
    const double symCollectMs = symCollectTimer.ElapsedMs();

    // 4. SemanticAnalyzer::Analyze (All Checkers)
    i18n::I18n i18n;
    std::shared_ptr<const analysis::Scope> sharedScope = std::move(scopeRoot);
    const std::string docUri = "file:///massive_benchmark.as";
    analysis::SemanticAnalysisRequest semReq(symbolTable, docUri, "", &i18n);
    semReq.sourceCode = massiveCode;
    semReq.tree = tree;
    semReq.scopeRoot = sharedScope;
    semReq.nodeIndex = &nodeIndex;

    analysis::SemanticAnalyzer analyzer(nullptr);
    HighResTimer semTimer;
    auto diagnostics = analyzer.Analyze(semReq);
    const double semMs = semTimer.ElapsedMs();

    // 5. SemanticTokens
    features::SemanticTokensRequest tokReq(docUri, massiveCode, tree, symbolTable);
    tokReq.scopeRoot = sharedScope;
    tokReq.nodeIndex = &nodeIndex;

    HighResTimer tokTimer;
    auto tokens = features::GetSemanticTokens(tokReq);
    const double tokMs = tokTimer.ElapsedMs();

    // 6. FindInnermostScope 10,000 queries
    HighResTimer scopeLookupTimer;
    const analysis::Scope* foundScope = nullptr;
    for (uint32_t q = 0; q < 10000; ++q)
    {
        uint32_t line = (q * 17) % static_cast<uint32_t>(actualLines);
        foundScope = analysis::FindInnermostScope(sharedScope.get(), line, 10);
    }
    const double scopeLookupMs = scopeLookupTimer.ElapsedMs();

    // 7. GetHover in massive file
    size_t fnPos = massiveCode.find("Compute");
    REQUIRE(fnPos != std::string::npos);
    uint32_t fnRow = static_cast<uint32_t>(std::count(massiveCode.begin(), massiveCode.begin() + fnPos, '\n'));
    size_t lastNl = massiveCode.rfind('\n', fnPos);
    uint32_t fnCol = static_cast<uint32_t>(fnPos - (lastNl == std::string::npos ? 0 : lastNl + 1));
    analysis::ScopeIndex bScopeIndex;
    bScopeIndex.SetScopeTree(docUri, sharedScope);
    features::HoverRequest bHoverReq{docUri, massiveCode, tree, symbolTable, bScopeIndex, lsp::Position{fnRow, fnCol}};
    HighResTimer bHoverTimer;
    auto bHoverRes = features::GetHover(bHoverReq);
    const double bHoverMs = bHoverTimer.ElapsedMs();

    MESSAGE("Scenario B (Massive Script: " << massiveCode.size() << " bytes, " << actualLines << " lines):");
    MESSAGE("  - AST Parse: " << parseMs << " ms");
    MESSAGE("  - NodeIndex::Build: " << indexMs << " ms");
    MESSAGE("  - CollectScopesFromTree: " << scopeMs << " ms");
    MESSAGE("  - CollectSymbolsWithTree: " << symCollectMs << " ms");
    MESSAGE("  - SemanticAnalyzer::Analyze: " << semMs << " ms");
    MESSAGE("  - SemanticTokens: " << tokMs << " ms (Token count: " << tokens.data.size() / 5 << ")");
    MESSAGE("  - 10,000 FindInnermostScope queries: " << scopeLookupMs << " ms");
    MESSAGE("  - GetHover (Compute): " << bHoverMs << " ms");

    CHECK(foundScope != nullptr);
    CHECK(bHoverRes.has_value());

    ts_tree_delete(tree);
}

TEST_CASE("Benchmark - Scenario C: Large Multi-File Project with Massive #include DAG" * doctest::skip(true))
{
    BenchmarkSandbox sandbox;
    std::mt19937_64 rng(777);
    constexpr size_t kFileCount = 35;
    const auto files = CreateIncludeProject(sandbox, kFileCount, rng);
    REQUIRE(files.size() == kFileCount);

    WorkspaceIncludeGraph graph;
    HighResTimer scanTimer;
    graph.BuildFromFiles(files, {sandbox.rootDir.string()}, {sandbox.rootDir.string()});
    const double scanMs = scanTimer.ElapsedMs();

    CHECK(graph.FileCount() == kFileCount);

    // Invalidation traversal
    HighResTimer invalidationTimer;
    const auto affected = graph.GetReverseDependenciesTopological(files.front());
    const double invalidationMs = invalidationTimer.ElapsedMs();

    // Cross-#include GetHover
    const auto sharedPath = sandbox.rootDir / "CoreShared.as";
    const std::string sharedContent = ReadEntireFile(sharedPath);
    std::string testScript = "#include \"CoreShared.as\"\nvoid TestHover() { CoreShared cs; cs.Init(); }\n";
    parser::AngelScriptParser cParser;
    TSTree* cTree = cParser.Parse(testScript);
    REQUIRE(cTree != nullptr);
    TSTree* sharedTree = cParser.Parse(sharedContent);
    REQUIRE(sharedTree != nullptr);
    analysis::SymbolTable cTable;
    analysis::SymbolCollector symbolCollector(nullptr);
    const std::string sharedUri = utils::PathToUri(IncludeResolver::NormalizePath(sharedPath));
    symbolCollector.CollectSymbolsWithTree(analysis::SymbolCollectRequest(sharedUri, sharedContent), sharedTree,
                                           cTable);
    symbolCollector.CollectSymbolsWithTree(analysis::SymbolCollectRequest("file:///test.as", testScript), cTree,
                                           cTable);
    analysis::ScopeIndex cScopeIndex;
    features::HoverRequest cHoverReq{
        "file:///test.as", testScript, cTree, cTable, cScopeIndex, lsp::Position{1, 22} // 'CoreShared' inside TestHover
    };
    HighResTimer cHoverTimer;
    auto cHoverRes = features::GetHover(cHoverReq);
    const double cHoverMs = cHoverTimer.ElapsedMs();

    MESSAGE("Scenario C (DAG with " << kFileCount << " files):");
    MESSAGE("  - Graph Population: " << scanMs << " ms");
    MESSAGE("  - Invalidation of Root Node: " << invalidationMs << " ms (Affected: " << affected.size() << ")");
    MESSAGE("  - Cross-#include GetHover: " << cHoverMs << " ms");

    CHECK(affected.size() >= kFileCount - 1);
    CHECK(cHoverRes.has_value());

    ts_tree_delete(cTree);
    ts_tree_delete(sharedTree);
}
