#include <doctest/doctest.h>

#include "analysis/EngineProfiles.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "lsp/ModuleIndex.h"
#include "parser/AngelScriptParser.h"
#include "utils/Timer.h"
#include "utils/IncludeResolver.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace angel_lsp::utils;

struct TempDirectoryGuard
{
    std::filesystem::path dir;

    TempDirectoryGuard()
    {
        const std::string name = angel_lsp::test::GenerateRandomSymbolName("perf_dag");
        dir = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(dir);
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(dir, ec);
        if (!ec)
            dir = std::move(canon);
    }

    ~TempDirectoryGuard()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string WriteScript(const std::string& name, const std::string& content) const
    {
        const auto full = dir / name;
        std::ofstream out(full, std::ios::binary);
        out << content;
        return IncludeResolver::NormalizePath(full);
    }
};

/**
 * @brief Constructs a randomized DAG of script files with diamond inclusions.
 */
std::vector<std::string> GenerateRandomDAG(const TempDirectoryGuard& temp, size_t count, std::mt19937_64& rng)
{
    std::vector<std::string> fileNames;
    fileNames.reserve(count);
    for (size_t i = 0; i < count; ++i)
        fileNames.push_back("node_" + std::to_string(i) + ".as");

    std::vector<std::string> fullPaths;
    fullPaths.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        std::string content;
        if (i > 0)
        {
            std::uniform_int_distribution<size_t> parentDist(0, i - 1);
            size_t parent1 = parentDist(rng);
            content += "#include \"" + fileNames[parent1] + "\"\n";
            if (i > 1)
            {
                size_t parent2 = parentDist(rng);
                if (parent2 != parent1)
                    content += "#include \"" + fileNames[parent2] + "\"\n";
            }
        }
        content += "void fn_" + std::to_string(i) + "() {}\n";
        fullPaths.push_back(temp.WriteScript(fileNames[i], content));
    }
    return fullPaths;
}

} // namespace

TEST_CASE("Performance - PERF-01 Randomized DAG Topological Invalidation Invariant")
{
    TempDirectoryGuard temp;
    std::mt19937_64 rng(42);
    constexpr size_t k_nodeCount = 200;

    auto scriptPaths = GenerateRandomDAG(temp, k_nodeCount, rng);

    WorkspaceIncludeGraph graph;
    const std::string rootNorm = IncludeResolver::NormalizePath(temp.dir);
    graph.Build({rootNorm}, {}, ".as");

    const std::string rootNode = scriptPaths[0];

    const auto start = std::chrono::steady_clock::now();
    auto revDeps = graph.GetReverseDependenciesTopological(rootNode);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    // 1. Zero duplicates invariant
    std::vector<std::string> deduplicated = revDeps;
    std::sort(deduplicated.begin(), deduplicated.end());
    CHECK(std::adjacent_find(deduplicated.begin(), deduplicated.end()) == deduplicated.end());

    // 2. Topological ordering invariant
    ankerl::unordered_dense::map<std::string, size_t> positionMap;
    for (size_t i = 0; i < revDeps.size(); ++i)
        positionMap[revDeps[i]] = i;

    for (size_t i = 0; i < revDeps.size(); ++i)
    {
        auto includers = graph.GetFilesIncluding(revDeps[i]);
        for (const auto& inc : includers)
        {
            if (positionMap.contains(inc))
                CHECK(positionMap[revDeps[i]] < positionMap[inc]);
        }
    }

    // 3. Performance budget < 15 ms
    CHECK(elapsed.count() < 15);
}

TEST_CASE("Performance - PERF-03 Randomized Prefix Lookup Benchmark Invariant")
{
    std::mt19937_64 rng(1337);
    constexpr size_t k_symbolCount = 10000;
    constexpr size_t k_queryCount = 500;

    std::vector<angel_lsp::ModuleIndex::ExportedSymbol> symbols;
    symbols.reserve(k_symbolCount);
    std::vector<std::string> prefixes = {"var_", "func_", "cls_", "prop_", "field_"};

    for (size_t i = 0; i < k_symbolCount; ++i)
    {
        const std::string prefix = prefixes[i % prefixes.size()];
        const std::string symName = prefix + std::to_string(i);
        symbols.push_back(angel_lsp::ModuleIndex::ExportedSymbol{symName, "NS::" + symName, "file:///doc.as",
                                                                 static_cast<uint32_t>(i), 0});
    }

    angel_lsp::ModuleIndex index;
    index.SetExportedSymbols(std::move(symbols));

    const auto start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < k_queryCount; ++q)
    {
        const std::string query = prefixes[q % prefixes.size()] + std::to_string(q % 100);
        auto matches = index.FindSymbolsByPrefix(query);
        for (const auto& sym : matches)
            CHECK(sym.name.starts_with(query));
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

    const double avgMs = (static_cast<double>(elapsed.count()) / 1000.0) / static_cast<double>(k_queryCount);
    CHECK(avgMs < 1.5);
}

namespace
{
/**
 * @brief Constructs a realistic weapon script of approximately 380 lines.
 * @param[in] className Name of the weapon class.
 * @return Formatted AngelScript code string.
 */
std::string BuildWeaponScript(const std::string& className)
{
    std::string script;
    script.reserve(16384);
    script += "class " + className + " : CBasePlayerWeapon\n{\n";
    script += "    CBasePlayerWeapon@ self;\n";
    script += "    int m_iShotsFired;\n";
    script += "    float m_flNextPrimaryAttack;\n";
    script += "    bool m_bInReload;\n\n";

    script += "    void Spawn()\n    {\n";
    script += "        Precache();\n";
    script += "        g_EntityFuncs.SetModel(self, \"models/v_weapon.mdl\");\n";
    script += "        self.m_iDefaultAmmo = 30;\n";
    script += "        self.m_iClip = 10;\n";
    script += "        self.PlayEmptySound();\n";
    script += "    }\n\n";

    script += "    void Precache()\n    {\n";
    script += "        g_SoundSystem.PlaySound(self, 1, \"weapons/shoot.wav\", 1.0f, 1.0f);\n";
    script += "    }\n\n";

    script += "    bool Deploy()\n    {\n";
    script += "        return self.PlayEmptySound();\n";
    script += "    }\n\n";

    script += "    void PrimaryAttack()\n    {\n";
    script += "        CBasePlayer@ pPlayer = g_PlayerFuncs.FindPlayerByIndex(1);\n";
    script += "        if (pPlayer is null)\n            return;\n";
    script += "        m_iShotsFired++;\n";
    script += "        self.m_iClip = self.m_iClip - 1;\n";
    script += "    }\n\n";

    script += "    void SecondaryAttack()\n    {\n";
    script += "        m_flNextPrimaryAttack = 0.5f;\n";
    script += "    }\n\n";

    script += "    void Reload()\n    {\n";
    script += "        m_bInReload = true;\n";
    script += "    }\n\n";

    script += "    void WeaponIdle()\n    {\n";
    script += "        if (m_bInReload)\n            m_bInReload = false;\n";
    script += "    }\n";

    for (int i = 0; i < 35; ++i)
    {
        script += "\n    void HelperAction_" + std::to_string(i) + "(int pVal)\n    {\n";
        script += "        int localAccum = pVal * 2;\n";
        script += "        if (localAccum > 100)\n";
        script += "            m_iShotsFired += localAccum;\n";
        script += "        else\n";
        script += "            m_iShotsFired -= 1;\n";
        script += "    }\n";
    }
    script += "}\n";
    return script;
}
} // namespace

TEST_CASE("Performance - PERF-02 Checkers Latency and Stub Invariant on Weapon Script")
{
    const std::string className = angel_lsp::test::GenerateRandomSymbolName("weapon_ins");
    const std::string weaponCode = BuildWeaponScript(className);
    const size_t lineCount = static_cast<size_t>(std::count(weaponCode.begin(), weaponCode.end(), '\n') + 1);

    angel_lsp::analysis::SymbolTable table;
    angel_lsp::parser::AngelScriptParser stubParser;
    angel_lsp::analysis::SymbolCollector stubCollector(nullptr);
    const std::string svenStub =
        angel_lsp::analysis::GetProfileStubText(angel_lsp::analysis::EngineProfileKind::SvenCoop);
    stubCollector.CollectSymbols("predefined:///svencoop.as.predefined", svenStub, stubParser, table);

    const std::string weaponUri = "file:///weapon_ins2coach.as";
    angel_lsp::parser::AngelScriptParser weaponParser;
    stubCollector.CollectSymbols(weaponUri, weaponCode, weaponParser, table);

    angel_lsp::parser::AngelScriptParser scopeParser;
    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopeRoot = scopeCollector.CollectScopes(weaponCode, scopeParser);

    angel_lsp::parser::AngelScriptParser treeParser;
    auto tree = treeParser.Parse(weaponCode);

    angel_lsp::i18n::I18n i18n;
    angel_lsp::analysis::SemanticAnalysisRequest req{table, weaponUri, weaponCode, &i18n};
    req.tree = std::move(tree);
    req.scopeRoot = std::move(scopeRoot);

    angel_lsp::analysis::SemanticAnalyzer analyzer(nullptr);

    angel_lsp::utils::HighResTimer timer;
    auto diagnostics = analyzer.Analyze(req);
    const double elapsedMs = timer.ElapsedMs();

    MESSAGE("Checkers latency on " << lineCount << " lines (weapon_ins2coach.as equivalent): " << elapsedMs << " ms");

    CHECK(elapsedMs < 50.0);

    for (const auto& diag : diagnostics)
    {
        if (diag.code == "as-warn-undeclared-identifier")
        {
            CHECK(diag.message.find("self") == std::string::npos);
        }
        if (diag.code == "as-err-unresolved-type")
        {
            CHECK(diag.message.find("CBasePlayerWeapon") == std::string::npos);
        }
    }
}
