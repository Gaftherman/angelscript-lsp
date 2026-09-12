#include <doctest/doctest.h>

#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "config/ServerConfig.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace
{
    /**
     * @brief Queries peak resident memory (Working Set / RSS) of current process in bytes.
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
     * @brief Escapes characters for JSON string payloads.
     */
    std::string JsonEscape(const std::string &text)
    {
        std::string escaped;
        escaped.reserve(text.size() + 16);
        for (const char c : text)
        {
            switch (c)
            {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped.push_back(c); break;
            }
        }
        return escaped;
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
            dir = std::filesystem::temp_directory_path() / ("angel_lsp_stress_" + unique);
            std::filesystem::create_directories(dir);
        }

        ~TempStressWorkspace()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &contents) const
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

        std::string Uri(const std::string &name) const
        {
            return angel_lsp::utils::PathToUri((dir / name).string());
        }

        std::string RootUri() const
        {
            return angel_lsp::utils::PathToUri(dir.string());
        }
    };
}

TEST_CASE("Server - Concurrency Stress Test: 500 rapid edits with interleaved reads")
{
    namespace fs = std::filesystem;

    fs::path repoRoot(ANGELSCRIPT_REPO_ROOT);
    fs::path largeScriptPath = repoRoot / "server" / "tests" / "perf" / "large_script_3000.as";
    fs::path svenStubPath = repoRoot / "predefined" / "sven.as.predefined";

    REQUIRE_MESSAGE(fs::exists(largeScriptPath), "large_script_3000.as must exist");

    std::string largeCode;
    {
        std::ifstream file(largeScriptPath, std::ios::binary);
        REQUIRE(file.is_open());
        std::ostringstream ss;
        ss << file.rdbuf();
        largeCode = ss.str();
    }
    REQUIRE(!largeCode.empty());

    TempStressWorkspace ws;
    ws.Write("large_script.as", largeCode);
    const std::string fileUri = ws.Uri("large_script.as");

    angel_lsp::config::ServerConfig serverConfig;
    serverConfig.searchDirectories.push_back(ws.dir.string());
    if (fs::exists(svenStubPath))
    {
        serverConfig.predefinedFiles.push_back(svenStubPath.string());
    }
    serverConfig.features.enableHover = true;
    serverConfig.features.enableSemanticTokens = true;
    serverConfig.features.enablePullDiagnostics = true;

    angel_lsp::test::ScriptedStream stream;
    int nextReqId = 1;

    // 1. Initialize
    const int initId = nextReqId++;
    stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(initId) +
                R"(,"method":"initialize","params":{"processId":null,"rootUri":")" +
                ws.RootUri() + R"(","capabilities":{},"workspaceFolders":[{"uri":")" +
                ws.RootUri() + R"(","name":"fixture"}]}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // 2. Open document
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
                fileUri + R"(","languageId":"angelscript","version":1,"text":")" +
                JsonEscape(largeCode) + R"("}}})");

    std::vector<int> hoverRequestIds;
    std::vector<int> tokensRequestIds;
    std::vector<int> diagnosticRequestIds;

    constexpr int kEditCount = 500;
    // Initial line 0 is: "// Large AngelScript Benchmark Script (3000 lines)" (49 characters)
    int currentLine0Length = 49;

    // 3. Loop 500 rapid edits with interleaved reads
    for (int i = 0; i < kEditCount; ++i)
    {
        const int version = i + 2;
        const std::string replacement = "// Stress iteration " + std::to_string(i) + " comment line padding";

        std::string didChangeJson = R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
                                    fileUri + R"(","version":)" + std::to_string(version) +
                                    R"(},"contentChanges":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":)" +
                                    std::to_string(currentLine0Length) + R"(}},"text":")" +
                                    JsonEscape(replacement) + R"("}]}})";
        stream.Push(didChangeJson);
        currentLine0Length = static_cast<int>(replacement.size());

        // Interleaved hover (every 10 edits)
        if (i % 10 == 0)
        {
            const int hoverId = nextReqId++;
            hoverRequestIds.push_back(hoverId);
            stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(hoverId) +
                        R"(,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
                        fileUri + R"("},"position":{"line":5,"character":11}}})");
        }

        // Interleaved semantic tokens (every 25 edits)
        if (i % 25 == 0)
        {
            const int tokensId = nextReqId++;
            tokensRequestIds.push_back(tokensId);
            stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(tokensId) +
                        R"(,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":")" +
                        fileUri + R"("}}})");
        }

        // Interleaved pull diagnostics (every 50 edits)
        if (i % 50 == 0)
        {
            const int diagId = nextReqId++;
            diagnosticRequestIds.push_back(diagId);
            stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(diagId) +
                        R"(,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                        fileUri + R"("}}})");
        }
    }

    // 4. Final hover request targeting EntityBase_0 at line 22, char 8
    const int finalHoverId = nextReqId++;
    stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(finalHoverId) +
                R"(,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
                fileUri + R"("},"position":{"line":22,"character":8}}})");

    // 5. Barrier allowing background analysis to settle
    stream.PushAction([&stream]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });

    // 6. Shutdown and Exit
    const int shutdownId = nextReqId++;
    stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(shutdownId) + R"(,"method":"shutdown"})");
    stream.Push(R"({"jsonrpc":"2.0","method":"exit"})");

    const size_t memBefore = GetPeakWorkingSetBytes();
    angel_lsp::utils::HighResTimer timer;

    {
        angel_lsp::Server server(serverConfig, stream);
        server.Run();
    }

    const double elapsedMs = timer.ElapsedMs();
    const size_t memAfter = GetPeakWorkingSetBytes();

    MESSAGE("500 edits stress test completed in ", elapsedMs, " ms");
    MESSAGE("Peak working set before: ", memBefore / (1024 * 1024), " MB, after: ", memAfter / (1024 * 1024), " MB");

    // Assertions
    CHECK(stream.ResponseFor(initId).find("\"capabilities\"") != std::string::npos);

    for (int hId : hoverRequestIds)
    {
        const std::string resp = stream.ResponseFor(hId);
        CHECK_FALSE(resp.empty());
        CHECK(resp.find("\"error\"") == std::string::npos);
    }

    for (int tId : tokensRequestIds)
    {
        const std::string resp = stream.ResponseFor(tId);
        CHECK_FALSE(resp.empty());
        CHECK(resp.find("\"error\"") == std::string::npos);
        CHECK(resp.find("\"data\"") != std::string::npos);
    }

    for (int dId : diagnosticRequestIds)
    {
        const std::string resp = stream.ResponseFor(dId);
        CHECK_FALSE(resp.empty());
    }

    const std::string finalHoverResp = stream.ResponseFor(finalHoverId);
    CHECK(finalHoverResp.find("EntityBase_0") != std::string::npos);

    CHECK(stream.ResponseFor(shutdownId).find("\"result\":null") != std::string::npos);
}
