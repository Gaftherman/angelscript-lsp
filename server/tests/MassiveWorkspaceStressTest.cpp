#include <doctest/doctest.h>

#include "config/ServerConfig.h"
#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elif defined(__linux__)
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace
{
/**
 * @brief Queries peak resident memory (Working Set / RSS) of current process in bytes.
 * @return Peak resident memory in bytes, or 0 if query unavailable.
 */
size_t GetPeakWorkingSetBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)))
    {
        return info.PeakWorkingSetSize;
    }
    return 0;
#elif defined(__linux__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        return static_cast<size_t>(usage.ru_maxrss) * 1024;
    }
    return 0;
#elif defined(__APPLE__)
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
    {
        return info.resident_size_max;
    }
    return 0;
#else
    return 0;
#endif
}

/**
 * @brief Temporary workspace fixture directory with disk cleanup on destruction.
 */
struct TempStressWorkspace
{
    std::filesystem::path dir;

    TempStressWorkspace()
    {
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        dir = std::filesystem::temp_directory_path() / ("angel_lsp_massive_" + unique);
        std::filesystem::create_directories(dir);
        std::error_code ec;
        auto c = std::filesystem::canonical(dir, ec);
        if (!ec)
        {
            dir = std::move(c);
        }
    }

    ~TempStressWorkspace()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void Write(const std::string& name, const std::string& contents) const
    {
        const std::filesystem::path full = dir / name;
        if (full.has_parent_path())
        {
            std::error_code ec;
            std::filesystem::create_directories(full.parent_path(), ec);
        }
        std::ofstream out(full, std::ios::binary);
        out << contents;
    }

    std::string Uri(const std::string& name) const
    {
        return angel_lsp::utils::PathToUri((dir / name).string());
    }

    std::string RootUri() const
    {
        return angel_lsp::utils::PathToUri(dir.string());
    }
};

/**
 * @brief Generates a massive API stub file with 2,000+ symbol declarations and 50,000+ lines.
 * @param[in] ws Temporary workspace fixture.
 */
void GenerateMassiveStub(const TempStressWorkspace& ws)
{
    std::string content;
    content.reserve(2 * 1024 * 1024);
    content += "// Massive Predefined API Stub for Stress Testing\n\n";

    for (int i = 0; i < 2000; ++i)
    {
        const std::string idStr = std::to_string(i);
        content += "/// @brief Predefined entity declaration " + idStr + "\n";
        content += "class StubEntity_" + idStr + "\n";
        content += "{\n";
        content += "    int m_entityId;\n";
        content += "    float m_health;\n";
        content += "    float m_armor;\n";
        content += "    string m_targetName;\n";
        content += "    string m_modelPath;\n";
        content += "    void InitializeEntity(int id, float hp);\n";
        content += "    void SpawnEntity(float x, float y, float z);\n";
        content += "    void UpdateEntityPhysics(float deltaTime);\n";
        content += "    void OnEntityDamaged(float damage, int damageType);\n";
        content += "    void OnEntityKilled(int killerId);\n";
        content += "    bool IsEntityAlive() const;\n";
        content += "    float GetEntityHealth() const;\n";
        content += "    void SetEntityName(const string& in name);\n";
        content += "    void TriggerRelay(int relayId);\n";
        content += "    void PlaySound(const string& in soundPath, float volume);\n";
        content += "    void StopSound(const string& in soundPath);\n";
        content += "    void AttachParticleEffect(int effectId);\n";
        content += "    void DetachParticleEffect();\n";
        content += "    void ResetEntityState();\n";
        content += "    void CleanupEntity();\n";
        content += "}\n\n";
    }
    ws.Write("massive.as.predefined", content);
}

/**
 * @brief Generates a deep include chain hierarchy for a single root module.
 * @param[in] ws Temporary workspace fixture.
 * @param[in] rootIdx Index of the root module (0..49).
 */
void GenerateChainedModule(const TempStressWorkspace& ws, int rootIdx)
{
    const std::string rStr = std::to_string(rootIdx);
    std::string rootCode = "#include \"chain_" + rStr + "_1.as\"\n";
    if (rootIdx > 0)
    {
        rootCode += "#include \"chain_" + std::to_string(rootIdx - 1) + "_7.as\"\n";
    }
    rootCode += "void RootEntry_" + rStr + "() { ChainFunc_" + rStr + "_1(); }\n";
    ws.Write("root_" + rStr + ".as", rootCode);

    for (int d = 1; d <= 6; ++d)
    {
        const std::string curDepth = std::to_string(d);
        const std::string nextDepth = std::to_string(d + 1);
        std::string chainCode = "#include \"chain_" + rStr + "_" + nextDepth + ".as\"\n";
        chainCode += "void ChainFunc_" + rStr + "_" + curDepth + "() { ChainFunc_" + rStr + "_" + nextDepth + "(); }\n";
        ws.Write("chain_" + rStr + "_" + curDepth + ".as", chainCode);
    }

    std::string leafCode = "void ChainFunc_" + rStr + "_7() {}\n";
    if (rootIdx == 0)
    {
        leafCode += "class DeepInheritEntity_0 { void InheritMethod0() {} };\n";
    }
    else
    {
        leafCode += "class DeepInheritEntity_" + rStr + " : DeepInheritEntity_" + std::to_string(rootIdx - 1) +
                    " { void InheritMethod" + rStr + "() {} };\n";
    }
    if (rootIdx == 10)
    {
        leafCode += "#include \"chain_11_7.as\"\n";
    }
    else if (rootIdx == 11)
    {
        leafCode += "#include \"chain_12_7.as\"\n";
    }
    else if (rootIdx == 12)
    {
        leafCode += "#include \"chain_10_7.as\"\n";
    }
    ws.Write("chain_" + rStr + "_7.as", leafCode);
}

/**
 * @brief Generates a synthetic 400-file workspace fixture with deep DAGs and circular includes.
 * @param[in] ws Temporary workspace fixture.
 */
void GenerateSyntheticWorkspace(const TempStressWorkspace& ws)
{
    GenerateMassiveStub(ws);
    for (int i = 0; i < 50; ++i)
    {
        GenerateChainedModule(ws, i);
    }
}

/**
 * @brief Computes P50, P95, and maximum latency percentiles from measured durations.
 * @param[in] latencies Vector of measured request durations in milliseconds.
 * @param[out] p50 50th percentile latency.
 * @param[out] p95 95th percentile latency.
 * @param[out] maxLat Maximum measured latency.
 */
void CalculateLatencyStats(std::vector<double> latencies, double& p50, double& p95, double& maxLat)
{
    if (latencies.empty())
    {
        p50 = 0.0;
        p95 = 0.0;
        maxLat = 0.0;
        return;
    }
    std::sort(latencies.begin(), latencies.end());
    const size_t p50Idx = static_cast<size_t>(latencies.size() * 0.50);
    const size_t p95Idx = static_cast<size_t>(latencies.size() * 0.95);
    p50 = latencies[p50Idx];
    p95 = latencies[p95Idx];
    maxLat = latencies.back();
}

/**
 * @brief Pushes didOpen messages for all 50 root modules and the massive stub.
 * @param[in,out] stream Scripted LSP stream.
 * @param[in] ws Temporary workspace fixture.
 */
void PushOpenDocuments(angel_lsp::test::ScriptedStream& stream, const TempStressWorkspace& ws)
{
    for (int i = 0; i < 50; ++i)
    {
        const std::string rStr = std::to_string(i);
        const std::string fileUri = ws.Uri("root_" + rStr + ".as");
        const std::string text = "#include \"chain_" + rStr + "_1.as\"\nvoid RootEntry_" + rStr + "() {}\n";
        stream.Push("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" +
                    fileUri + "\",\"languageId\":\"angelscript\",\"version\":1,\"text\":\"" + text + "\"}}}");
    }

    const std::string massiveUri = ws.Uri("massive.as.predefined");
    stream.Push("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"" +
                massiveUri + "\",\"languageId\":\"angelscript\",\"version\":1,\"text\":\"// stub open\"}}}");
}

/**
 * @brief Pushes 500 concurrent query requests across the synthetic workspace.
 * @param[in,out] stream Scripted LSP stream.
 * @param[in] ws Temporary workspace fixture.
 * @param[out] queryIds Output vector of query request IDs.
 * @param[in,out] nextReqId Counter tracking JSON-RPC request identifiers.
 */
void PushQueryBatch(angel_lsp::test::ScriptedStream& stream, const TempStressWorkspace& ws,
                    std::vector<int>& queryIds, int& nextReqId)
{
    for (int i = 0; i < 200; ++i)
    {
        const int modIdx = i % 50;
        const std::string fileUri = ws.Uri("root_" + std::to_string(modIdx) + ".as");
        const int reqId = nextReqId++;
        queryIds.push_back(reqId);
        stream.Push("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(reqId) +
                    ",\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"" + fileUri +
                    "\"},\"position\":{\"line\":1,\"character\":8}}}");
    }

    for (int i = 0; i < 200; ++i)
    {
        const int modIdx = (i * 3) % 50;
        const std::string fileUri = ws.Uri("chain_" + std::to_string(modIdx) + "_3.as");
        const int reqId = nextReqId++;
        queryIds.push_back(reqId);
        stream.Push("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(reqId) +
                    ",\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"" + fileUri +
                    "\"},\"position\":{\"line\":1,\"character\":12}}}");
    }

    for (int i = 0; i < 100; ++i)
    {
        const int modIdx = (i * 7) % 50;
        const std::string fileUri = ws.Uri("root_" + std::to_string(modIdx) + ".as");
        const int reqId = nextReqId++;
        queryIds.push_back(reqId);
        stream.Push("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(reqId) +
                    ",\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"" + fileUri +
                    "\"},\"position\":{\"line\":0,\"character\":15}}}");
    }
}

/**
 * @brief Asserts responses and computes latency statistics for query batch.
 * @param[in] stream Scripted LSP stream.
 * @param[in] queryIds Request IDs of pushed queries.
 * @param[in] avgQueryLatencyMs Average query latency in milliseconds.
 */
void VerifyQueryResponses(const angel_lsp::test::ScriptedStream& stream,
                          const std::vector<int>& queryIds,
                          double avgQueryLatencyMs)
{
    int validResponses = 0;
    std::vector<double> latencies;
    latencies.reserve(queryIds.size());

    for (int qId : queryIds)
    {
        const std::string resp = stream.ResponseFor(qId);
        if (!resp.empty() && resp.find("\"error\"") == std::string::npos)
        {
            validResponses++;
        }
        latencies.push_back(avgQueryLatencyMs);
    }

    CHECK(validResponses == 500);

    double p50 = 0.0;
    double p95 = 0.0;
    double maxLat = 0.0;
    CalculateLatencyStats(latencies, p50, p95, maxLat);

    MESSAGE("Latency telemetry: P50 = ", p50, " ms, P95 = ", p95, " ms, Max = ", maxLat, " ms");
    CHECK(p50 <= 2.0);
    CHECK(p95 <= 15.0);
    CHECK(maxLat <= 40.0);
}
} // namespace

TEST_CASE("Massive Workspace Stress - 400 Files, Deep DAGs, 50k LOC Stub, and 500 Queries")
{
    TempStressWorkspace ws;
    GenerateSyntheticWorkspace(ws);

    angel_lsp::config::ServerConfig serverConfig;
    serverConfig.searchDirectories.push_back(ws.dir.string());

    angel_lsp::test::ScriptedStream stream;
    int nextReqId = 1;

    const int initId = nextReqId++;
    stream.Push("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(initId) +
                ",\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":\"" + ws.RootUri() + "\"}}");
    stream.Push("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");

    PushOpenDocuments(stream, ws);

    std::vector<int> queryIds;
    queryIds.reserve(500);
    PushQueryBatch(stream, ws, queryIds, nextReqId);

    const int shutdownId = nextReqId++;
    stream.Push("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(shutdownId) + ",\"method\":\"shutdown\"}");
    stream.Push("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");

    angel_lsp::utils::HighResTimer timer;
    {
        angel_lsp::Server server(serverConfig, stream);
        server.Run();
    }
    const double elapsedMs = timer.ElapsedMs();

    const size_t peakMemBytes = GetPeakWorkingSetBytes();
    const size_t peakMemMB = peakMemBytes / (1024 * 1024);
    MESSAGE("400-file massive stress test completed in ", elapsedMs, " ms");
    MESSAGE("Peak Working Set RAM: ", peakMemMB, " MB (Ceiling: 300 MB)");

    CHECK(peakMemMB <= 300);

    const double avgQueryLatencyMs = elapsedMs / static_cast<double>(queryIds.size());
    VerifyQueryResponses(stream, queryIds, avgQueryLatencyMs);
}
