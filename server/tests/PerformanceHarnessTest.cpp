#include <doctest/doctest.h>

#include "helpers/TestUtils.h"
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

struct SyntheticSymbol
{
    std::string name;
    std::string qualifiedName;
    uint32_t line = 0;
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

/**
 * @brief Performs binary search for symbols having a specified prefix.
 */
std::vector<const SyntheticSymbol*> FindSymbolsByPrefix(const std::vector<SyntheticSymbol>& symbols,
                                                        std::string_view prefix)
{
    auto it = std::lower_bound(symbols.begin(), symbols.end(), prefix,
                               [](const SyntheticSymbol& sym, std::string_view p) { return sym.name < p; });

    std::vector<const SyntheticSymbol*> matches;
    while (it != symbols.end() && it->name.starts_with(prefix))
    {
        matches.push_back(&(*it));
        ++it;
    }
    return matches;
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

    std::vector<SyntheticSymbol> symbols;
    symbols.reserve(k_symbolCount);
    std::vector<std::string> prefixes = {"var_", "func_", "cls_", "prop_", "field_"};

    for (size_t i = 0; i < k_symbolCount; ++i)
    {
        const std::string prefix = prefixes[i % prefixes.size()];
        const std::string symName = prefix + std::to_string(i);
        symbols.push_back(SyntheticSymbol{symName, "NS::" + symName, static_cast<uint32_t>(i)});
    }

    std::sort(symbols.begin(), symbols.end(),
              [](const SyntheticSymbol& a, const SyntheticSymbol& b) { return a.name < b.name; });

    const auto start = std::chrono::steady_clock::now();
    for (size_t q = 0; q < k_queryCount; ++q)
    {
        const std::string query = prefixes[q % prefixes.size()] + std::to_string(q % 100);
        auto matches = FindSymbolsByPrefix(symbols, query);
        for (const auto* sym : matches)
            CHECK(sym->name.starts_with(query));
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);

    const double avgMs = (static_cast<double>(elapsed.count()) / 1000.0) / static_cast<double>(k_queryCount);
    CHECK(avgMs < 1.5);
}
