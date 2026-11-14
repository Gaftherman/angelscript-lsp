#include "lsp/state/SyntaxRevisionCache.h"

#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <random>
#include <string>

namespace syntax_revision_cache_test
{
inline std::string GenerateAngelScriptSource(size_t targetLines, std::mt19937_64& rng)
{
    std::string code;
    code.reserve(targetLines * 48);
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("CacheNs");
    code += "namespace " + nsName + "\n{\n";
    size_t lines = 2;
    size_t idx = 0;
    std::uniform_int_distribution<int> coeffDist(2, 19);

    while (lines + 3 < targetLines)
    {
        const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Fn") + std::to_string(idx++);
        const int coeff = coeffDist(rng);
        code += "    int " + fnName + "(int a, int b)\n    {\n";
        code += "        int acc = (a + b) * " + std::to_string(coeff) + ";\n";
        code += "        if (acc > 10)\n        {\n";
        code += "            acc -= 3;\n";
        code += "        }\n";
        code += "        return acc + a;\n";
        code += "    }\n";
        lines += 9;
    }
    code += "}\n";
    return code;
}
} // namespace syntax_revision_cache_test

TEST_SUITE("lsp/SyntaxRevisionCache")
{
    TEST_CASE("SyntaxRevisionCache - Large open revision (>32768 nodes) is retained and reused")
    {
        constexpr uint64_t kSeed = 2026111401ULL;
        INFO("Seed: " << kSeed);
        std::mt19937_64 rng(kSeed);

        const std::string source = syntax_revision_cache_test::GenerateAngelScriptSource(5000, rng);
        const std::string uri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("large_open") + ".as";

        angel_lsp::parser::AngelScriptParser parser;
        angel_lsp::SyntaxRevisionCache cache;

        const angel_lsp::SyntaxRevision first =
            angel_lsp::ResolveSyntaxRevision({.key = {uri, source, 1}, .parser = parser}, &cache);
        REQUIRE(first.syntax != nullptr);
        REQUIRE(first.syntax->index.NodeCount() > 32768);
        CHECK(first.extractedNodes == first.syntax->index.NodeCount());
        CHECK(cache.EntryCount() == 1);
        CHECK(cache.Find({uri, source, 1}) == first.syntax);

        const angel_lsp::SyntaxRevision second =
            angel_lsp::ResolveSyntaxRevision({.key = {uri, source, 1}, .parser = parser}, &cache);
        CHECK(second.syntax == first.syntax);
        CHECK(second.extractedNodes == 0);
    }

    TEST_CASE("SyntaxRevisionCache - Exceeding byte budget evicts closed entries first, then oldest open")
    {
        constexpr uint64_t kSeed = 2026111402ULL;
        INFO("Seed: " << kSeed);
        std::mt19937_64 rng(kSeed);

        angel_lsp::parser::AngelScriptParser parser;

        const std::string uriOpenOld = "file:///" + angel_lsp::test::GenerateRandomSymbolName("open_old") + ".as";
        const std::string uriOpenNew = "file:///" + angel_lsp::test::GenerateRandomSymbolName("open_new") + ".as";
        const std::string uriClosedOld = "file:///" + angel_lsp::test::GenerateRandomSymbolName("closed_old") + ".as";
        const std::string uriClosedNew = "file:///" + angel_lsp::test::GenerateRandomSymbolName("closed_new") + ".as";
        const std::string uriOpenMid = "file:///" + angel_lsp::test::GenerateRandomSymbolName("open_mid") + ".as";
        const std::string uriOpenBig = "file:///" + angel_lsp::test::GenerateRandomSymbolName("open_big") + ".as";

        const std::string srcOpenOld = syntax_revision_cache_test::GenerateAngelScriptSource(40, rng);
        const std::string srcOpenNew = syntax_revision_cache_test::GenerateAngelScriptSource(40, rng);
        const std::string srcClosedOld = syntax_revision_cache_test::GenerateAngelScriptSource(40, rng);
        const std::string srcClosedNew = syntax_revision_cache_test::GenerateAngelScriptSource(40, rng);
        const std::string srcOpenMid = syntax_revision_cache_test::GenerateAngelScriptSource(22, rng);
        const std::string srcOpenBig = syntax_revision_cache_test::GenerateAngelScriptSource(95, rng);

        const auto synOpenOld =
            angel_lsp::ResolveSyntaxRevision({.key = {uriOpenOld, srcOpenOld, 1}, .parser = parser}).syntax;
        const auto synOpenNew =
            angel_lsp::ResolveSyntaxRevision({.key = {uriOpenNew, srcOpenNew, 1}, .parser = parser}).syntax;
        const auto synClosedOld =
            angel_lsp::ResolveSyntaxRevision({.key = {uriClosedOld, srcClosedOld, 0, 1}, .parser = parser}).syntax;
        const auto synClosedNew =
            angel_lsp::ResolveSyntaxRevision({.key = {uriClosedNew, srcClosedNew, 0, 1}, .parser = parser}).syntax;
        const auto synOpenMid =
            angel_lsp::ResolveSyntaxRevision({.key = {uriOpenMid, srcOpenMid, 1}, .parser = parser}).syntax;
        const auto synOpenBig =
            angel_lsp::ResolveSyntaxRevision({.key = {uriOpenBig, srcOpenBig, 1}, .parser = parser}).syntax;

        REQUIRE(synOpenOld != nullptr);
        REQUIRE(synOpenNew != nullptr);
        REQUIRE(synClosedOld != nullptr);
        REQUIRE(synClosedNew != nullptr);
        REQUIRE(synOpenMid != nullptr);
        REQUIRE(synOpenBig != nullptr);

        const size_t bOpenOld = synOpenOld->EstimateMemoryBytes(srcOpenOld.size());
        const size_t bOpenNew = synOpenNew->EstimateMemoryBytes(srcOpenNew.size());
        const size_t bClosedOld = synClosedOld->EstimateMemoryBytes(srcClosedOld.size());
        const size_t bClosedNew = synClosedNew->EstimateMemoryBytes(srcClosedNew.size());
        const size_t bOpenMid = synOpenMid->EstimateMemoryBytes(srcOpenMid.size());
        const size_t bOpenBig = synOpenBig->EstimateMemoryBytes(srcOpenBig.size());

        const size_t budget = bOpenOld + bOpenNew + bClosedOld + bClosedNew;
        REQUIRE(bOpenMid < bClosedOld);
        REQUIRE(bOpenOld + bOpenNew + bClosedNew + bOpenMid <= budget);
        REQUIRE(bOpenBig + bOpenMid <= budget);
        REQUIRE(bOpenBig + bOpenMid + bOpenNew + bOpenOld > budget);

        angel_lsp::SyntaxRevisionCache cache(/*maxEntries=*/8, /*maxRetainedBytes=*/budget);
        cache.Remember({uriOpenOld, srcOpenOld, 1}, synOpenOld);
        cache.Remember({uriOpenNew, srcOpenNew, 1}, synOpenNew);
        cache.Remember({uriClosedOld, srcClosedOld, 0, 1}, synClosedOld);
        cache.Remember({uriClosedNew, srcClosedNew, 0, 1}, synClosedNew);
        CHECK(cache.EntryCount() == 4);
        CHECK(cache.RetainedBytes() == budget);

        // Exceed budget slightly with a new open entry: only the oldest closed entry is evicted.
        cache.Remember({uriOpenMid, srcOpenMid, 1}, synOpenMid);
        CHECK_FALSE(cache.Find({uriClosedOld, srcClosedOld, 0, 1}));
        CHECK(cache.Find({uriClosedNew, srcClosedNew, 0, 1}) == synClosedNew);
        CHECK(cache.Find({uriOpenOld, srcOpenOld, 1}) == synOpenOld);
        CHECK(cache.Find({uriOpenNew, srcOpenNew, 1}) == synOpenNew);
        CHECK(cache.Find({uriOpenMid, srcOpenMid, 1}) == synOpenMid);
        CHECK(cache.RetainedBytes() <= budget);

        // Exceed budget further with a large open entry: remaining closed and oldest open entries are evicted.
        cache.Remember({uriOpenBig, srcOpenBig, 1}, synOpenBig);
        CHECK_FALSE(cache.Find({uriClosedNew, srcClosedNew, 0, 1}));
        CHECK_FALSE(cache.Find({uriOpenOld, srcOpenOld, 1}));
        CHECK(cache.Find({uriOpenMid, srcOpenMid, 1}) == synOpenMid);
        CHECK(cache.Find({uriOpenBig, srcOpenBig, 1}) == synOpenBig);
        CHECK(cache.RetainedBytes() <= budget);
    }

    TEST_CASE("SyntaxRevisionCache - Single revision exceeding byte budget is not retained")
    {
        constexpr uint64_t kSeed = 2026111403ULL;
        INFO("Seed: " << kSeed);
        std::mt19937_64 rng(kSeed);

        angel_lsp::parser::AngelScriptParser parser;
        const std::string smallUri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("small") + ".as";
        const std::string bigUri = "file:///" + angel_lsp::test::GenerateRandomSymbolName("oversized") + ".as";
        const std::string smallSource = syntax_revision_cache_test::GenerateAngelScriptSource(20, rng);
        const std::string bigSource = syntax_revision_cache_test::GenerateAngelScriptSource(100, rng);

        const auto smallSyntax =
            angel_lsp::ResolveSyntaxRevision({.key = {smallUri, smallSource, 1}, .parser = parser}).syntax;
        const auto bigSyntax =
            angel_lsp::ResolveSyntaxRevision({.key = {bigUri, bigSource, 1}, .parser = parser}).syntax;
        REQUIRE(smallSyntax != nullptr);
        REQUIRE(bigSyntax != nullptr);

        const size_t smallBytes = smallSyntax->EstimateMemoryBytes(smallSource.size());
        const size_t bigBytes = bigSyntax->EstimateMemoryBytes(bigSource.size());
        REQUIRE(bigBytes > smallBytes + 1);

        angel_lsp::SyntaxRevisionCache cache(/*maxEntries=*/8, /*maxRetainedBytes=*/bigBytes - 1);
        cache.Remember({smallUri, smallSource, 1}, smallSyntax);
        REQUIRE(cache.Find({smallUri, smallSource, 1}) == smallSyntax);

        cache.Remember({bigUri, bigSource, 1}, bigSyntax);
        CHECK_FALSE(cache.Find({bigUri, bigSource, 1}));
        CHECK(cache.Find({smallUri, smallSource, 1}) == smallSyntax);
        CHECK(cache.RetainedBytes() == smallBytes);
    }
}
