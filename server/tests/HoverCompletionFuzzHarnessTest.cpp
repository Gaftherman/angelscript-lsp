#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/SemanticCodeGenerator.h"
#include "helpers/TestUtils.h"
#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace angel_lsp::test;

namespace
{

struct LatencyStats
{
    double minMs = 0.0;
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
};

LatencyStats ComputeLatencyStats(std::vector<double>& samples)
{
    if (samples.empty())
    {
        return {};
    }
    std::sort(samples.begin(), samples.end());
    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    const size_t count = samples.size();
    LatencyStats stats;
    stats.minMs = samples.front();
    stats.meanMs = sum / static_cast<double>(count);
    stats.p50Ms = samples[count * 50 / 100];
    stats.p95Ms = samples[count * 95 / 100];
    stats.p99Ms = samples[count * 99 / 100];
    stats.maxMs = samples.back();
    return stats;
}

void RunCoordinateBombardment(LspSemanticHarnessFixture& fixture, const std::string& uri, size_t iterations,
                            std::mt19937_64& rng)
{
    std::uniform_int_distribution<uint32_t> lineDist(0, 100);
    std::uniform_int_distribution<uint32_t> colDist(0, 120);

    for (size_t i = 0; i < iterations; ++i)
    {
        const uint32_t l = lineDist(rng);
        const uint32_t c = colDist(rng);
        auto hover = fixture.RequestHover(uri, l, c);
        (void)hover;
        auto comp = fixture.RequestCompletion(uri, l, c);
        (void)comp;
    }
}

void CollectDurations(LspSemanticHarnessFixture& fixture, const std::string& uri, size_t count, std::mt19937_64& rng,
                      std::vector<double>& hoverDurations, std::vector<double>& compDurations)
{
    std::uniform_int_distribution<uint32_t> lineDist(0, 40);
    std::uniform_int_distribution<uint32_t> colDist(0, 60);

    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t l = lineDist(rng);
        const uint32_t c = colDist(rng);

        const auto hStart = std::chrono::steady_clock::now();
        auto hover = fixture.RequestHover(uri, l, c);
        (void)hover;
        const auto hEnd = std::chrono::steady_clock::now();
        hoverDurations.push_back(std::chrono::duration<double, std::milli>(hEnd - hStart).count());

        const auto cStart = std::chrono::steady_clock::now();
        auto comp = fixture.RequestCompletion(uri, l, c);
        (void)comp;
        const auto cEnd = std::chrono::steady_clock::now();
        compDurations.push_back(std::chrono::duration<double, std::milli>(cEnd - cStart).count());
    }
}

void VerifyPrimitiveFidelity(LspSemanticHarnessFixture& fixture, std::mt19937_64& rng)
{
    const PrimitiveScriptInfo prim = GeneratePrimitiveFunctionsScript(rng);
    const std::string primUri = fixture.SandboxUri("scripts/primitive_fidelity.as");
    fixture.AddVirtualDocument(primUri, prim.script);
    fixture.AssertHoverContains(primUri, prim.varLine, prim.varCol, prim.typeName);
}

void VerifyInheritanceFidelity(LspSemanticHarnessFixture& fixture, std::mt19937_64& rng)
{
    const InheritanceScriptInfo inher = GenerateInheritanceHierarchyScript(rng);
    const std::string inherUri = fixture.SandboxUri("scripts/inheritance_fidelity.as");
    fixture.AddVirtualDocument(inherUri, inher.script);

    fixture.AssertHoverContains(inherUri, inher.baseFieldHoverLine, inher.baseFieldHoverCol, "int");
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.baseField,
                                     lsp::CompletionItemKind::Field);
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.baseMethod,
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.derivedField,
                                     lsp::CompletionItemKind::Field);
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.derivedMethod,
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.leafField,
                                     lsp::CompletionItemKind::Field);
    fixture.AssertCompletionContains(inherUri, inher.dotLine, inher.dotCol, inher.leafMethod,
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionExcludes(inherUri, inher.dotLine, inher.dotCol, inher.baseName);
}

void VerifySvenCoopIntegration(LspSemanticHarnessFixture& fixture)
{
    fixture.LoadPredefinedStub("predefined/sven.as.predefined");

    std::mt19937_64 rng{5551212};
    const EngineScriptInfo eng = GenerateEngineHookScript(rng);
    const std::string engUri = fixture.SandboxUri("scripts/sven_engine_test.as");
    fixture.AddVirtualDocument(engUri, eng.script);

    fixture.AssertHoverContains(engUri, eng.playerHoverLine, eng.playerHoverCol, "CBasePlayer@");
    fixture.AssertCompletionContains(engUri, eng.playerDotLine, eng.playerDotCol, "pev",
                                     lsp::CompletionItemKind::Field);
    fixture.AssertCompletionContains(engUri, eng.playerDotLine, eng.playerDotCol, "Revive",
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionContains(engUri, eng.playerDotLine, eng.playerDotCol, "HasSuit",
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionContains(engUri, eng.playerDotLine, eng.playerDotCol, "IsAlive",
                                     lsp::CompletionItemKind::Method);
    fixture.AssertCompletionExcludes(engUri, eng.playerDotLine, eng.playerDotCol, eng.funcName);
}

void VerifyLatencyBudget(LspSemanticHarnessFixture& fixture, std::mt19937_64& rng)
{
    fixture.LoadPredefinedStub("predefined/sven.as.predefined");

    const std::string script = GenerateFuzzBombardmentScript(rng);
    const std::string uri = fixture.SandboxUri("scripts/perf_telemetry.as");
    fixture.AddVirtualDocument(uri, script);

    std::vector<double> hoverDurations;
    std::vector<double> compDurations;
    hoverDurations.reserve(1000);
    compDurations.reserve(1000);

    CollectDurations(fixture, uri, 1000, rng, hoverDurations, compDurations);

    const LatencyStats hoverStats = ComputeLatencyStats(hoverDurations);
    const LatencyStats compStats = ComputeLatencyStats(compDurations);

    MESSAGE("--- Hover Latency (ms) [N=1000] ---");
    MESSAGE("Min: " << hoverStats.minMs << " | Mean: " << hoverStats.meanMs << " | P50: " << hoverStats.p50Ms
                    << " | P95: " << hoverStats.p95Ms << " | P99: " << hoverStats.p99Ms << " | Max: "
                    << hoverStats.maxMs);

    MESSAGE("--- Completion Latency (ms) [N=1000] ---");
    MESSAGE("Min: " << compStats.minMs << " | Mean: " << compStats.meanMs << " | P50: " << compStats.p50Ms
                    << " | P95: " << compStats.p95Ms << " | P99: " << compStats.p99Ms << " | Max: "
                    << compStats.maxMs);

    CHECK(hoverStats.p95Ms <= 10.0);
    CHECK(compStats.p95Ms <= 15.0);
}

} // namespace

TEST_SUITE_BEGIN("HoverCompletionFuzzHarness");

TEST_CASE("Random Coordinate Bombardment - Zero Crash Invariant")
{
    LspSemanticHarnessFixture fixture;
    std::mt19937_64 rng{13374242};

    const std::string script = GenerateFuzzBombardmentScript(rng);
    const std::string uri = fixture.SandboxUri("scripts/fuzz_bombardment.as");
    fixture.AddVirtualDocument(uri, script);

    RunCoordinateBombardment(fixture, uri, 1000, rng);
    CHECK(true);
}

TEST_CASE("Semantic Fidelity - Primitive and Inheritance Chains")
{
    LspSemanticHarnessFixture fixture;
    std::mt19937_64 rng{987654321};
    VerifyPrimitiveFidelity(fixture, rng);
    VerifyInheritanceFidelity(fixture, rng);
}

TEST_CASE("Predefined Stubs Integration - Sven Co-op CBasePlayer")
{
    LspSemanticHarnessFixture fixture;
    VerifySvenCoopIntegration(fixture);
}

TEST_CASE("Latency & Performance Budget Telemetry")
{
    LspSemanticHarnessFixture fixture;
    std::mt19937_64 rng{777888999};
    VerifyLatencyBudget(fixture, rng);
}

TEST_SUITE_END();
