#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"
#include "parser/AngelScriptParser.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
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
 * @brief Queries the peak resident memory (Working Set / RSS) of the current process in bytes.
 * @return Peak resident memory in bytes, or 0 if unsupported.
 */
size_t GetPeakResidentMemoryBytes()
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
 * @brief Escapes characters for JSON strings.
 */
std::string JsonEscape(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

/**
 * @brief Temporary fixture directory with disk cleanup.
 */
struct TempWorkspace
{
    std::filesystem::path dir;

    TempWorkspace()
    {
        const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        dir = std::filesystem::temp_directory_path() / ("angel_lsp_perf_" + unique);
        std::filesystem::create_directories(dir);
        std::error_code ec;
        auto c = std::filesystem::canonical(dir, ec);
        if (!ec)
        {
            dir = std::move(c);
        }
    }

    ~TempWorkspace()
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
} // namespace

TEST_CASE("Perf - Baseline Measurements" * doctest::skip(true))
{
    namespace fs = std::filesystem;

    fs::path repoRoot(ANGELSCRIPT_REPO_ROOT);
    fs::path largeScriptPath = repoRoot / "server" / "tests" / "perf" / "large_script_3000.as";
    fs::path svenStubPath = repoRoot / "predefined" / "sven.as.predefined";

    REQUIRE_MESSAGE(fs::exists(largeScriptPath), "large_script_3000.as must exist");
    REQUIRE_MESSAGE(fs::exists(svenStubPath), "sven.as.predefined must exist");

    std::string largeCode;
    {
        std::ifstream file(largeScriptPath, std::ios::binary);
        REQUIRE(file.is_open());
        std::ostringstream ss;
        ss << file.rdbuf();
        largeCode = ss.str();
    }
    REQUIRE(!largeCode.empty());

    // -------------------------------------------------------------------------
    // 1. Measure stub load time for sven.as.predefined
    // -------------------------------------------------------------------------
    double stubLoadMs = 0.0;
    {
        angel_lsp::config::ServerConfig dummyConfig;
        angel_lsp::test::ScriptedStream dummyStream;
        angel_lsp::Server testServer(dummyConfig, dummyStream);
        angel_lsp::parser::AngelScriptParser parser;

        angel_lsp::utils::HighResTimer timer;
        testServer.ParserPredefined(svenStubPath.string(), parser, /*forceReload=*/true);
        stubLoadMs = timer.ElapsedMs();
    }

    // -------------------------------------------------------------------------
    // 2. Measure didOpen to first publishDiagnostics and debounced didChange
    // -------------------------------------------------------------------------
    double didOpenToDiagnosticsMs = 0.0;
    double debouncedDidChangeMs = 0.0;
    size_t peakResidentBytes = 0;

    {
        TempWorkspace ws;
        ws.Write("large_script.as", largeCode);
        const std::string fileUri = ws.Uri("large_script.as");

        angel_lsp::config::ServerConfig serverConfig;
        serverConfig.searchDirectories.push_back(ws.dir.string());
        serverConfig.predefinedFiles.push_back(svenStubPath.string());

        angel_lsp::test::ScriptedStream stream;
        stream.Push(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":")" +
                    ws.RootUri() + R"(","capabilities":{}}})");
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

        // Measure didOpen to first publishDiagnostics
        std::chrono::steady_clock::time_point openTime;
        std::chrono::steady_clock::time_point firstDiagTime;

        stream.PushAction([&openTime]() { openTime = std::chrono::steady_clock::now(); });

        stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" + fileUri +
                    R"(","languageId":"angelscript","version":1,"text":")" + JsonEscape(largeCode) + R"("}}})");

        // Wait for first publishDiagnostics on large_script.as
        stream.PushAction(
            [&stream, &firstDiagTime, &didOpenToDiagnosticsMs, &openTime]()
            {
                stream.WaitForCondition(
                    [&](const std::string& out)
                    {
                        if (out.find("textDocument/publishDiagnostics") != std::string::npos)
                        {
                            firstDiagTime = std::chrono::steady_clock::now();
                            didOpenToDiagnosticsMs =
                                std::chrono::duration<double, std::milli>(firstDiagTime - openTime).count();
                            return true;
                        }
                        return false;
                    },
                    std::chrono::seconds(60));
            });

        // Single character change: append a comment at the end
        std::string changedCode = largeCode + "\n// A single char edit\n";
        std::chrono::steady_clock::time_point changeTime;

        stream.PushAction([&changeTime]() { changeTime = std::chrono::steady_clock::now(); });

        stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
                    fileUri + R"(","version":2},"contentChanges":[{"text":")" + JsonEscape(changedCode) + R"("}]}})");

        angel_lsp::Server* serverPtr = nullptr;
        // Wait for debounced re-analysis output to stabilize
        stream.PushAction(
            [&serverPtr, &debouncedDidChangeMs, &changeTime]()
            {
                if (serverPtr)
                {
                    serverPtr->DrainQueue();
                }
                debouncedDidChangeMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - changeTime).count();
            });

        stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

        angel_lsp::Server server(serverConfig, stream);
        serverPtr = &server;
        server.Run();

        peakResidentBytes = GetPeakResidentMemoryBytes();
    }

    // -------------------------------------------------------------------------
    // 3. Measure GetSemanticTokens full and delta on large_script_3000.as
    // -------------------------------------------------------------------------
    double fullTokensMs = 0.0;
    double deltaTokensMs = 0.0;
    size_t tokenCount = 0;
    size_t editCount = 0;

    {
        angel_lsp::parser::AngelScriptParser parser;
        TSTree* tree = parser.Parse(largeCode);
        REQUIRE(tree != nullptr);

        angel_lsp::analysis::SymbolCollector collector(nullptr);
        angel_lsp::analysis::SymbolTable table;
        collector.CollectSymbols("file:///large_script.as", largeCode, parser, table);

        angel_lsp::features::SemanticTokensRequest reqFull{"file:///large_script.as", largeCode, tree, table};

        angel_lsp::utils::HighResTimer timerFull;
        auto fullResult = angel_lsp::features::GetSemanticTokens(reqFull);
        fullTokensMs = timerFull.ElapsedMs();
        tokenCount = fullResult.data.size() / 5;

        // Minor change for delta
        std::string changedCode = largeCode + "\nvoid ExtraFunction() { int y = 42; }\n";
        TSTree* changedTree = parser.Parse(changedCode);
        REQUIRE(changedTree != nullptr);

        angel_lsp::features::SemanticTokensRequest reqChanged{"file:///large_script.as", changedCode, changedTree,
                                                              table};
        auto changedResult = angel_lsp::features::GetSemanticTokens(reqChanged);

        angel_lsp::utils::HighResTimer timerDelta;
        auto edits = angel_lsp::features::ComputeSemanticTokensDelta(fullResult.data, changedResult.data);
        deltaTokensMs = timerDelta.ElapsedMs();
        editCount = edits.size();

        ts_tree_delete(tree);
        ts_tree_delete(changedTree);
    }

    // -------------------------------------------------------------------------
    // Output benchmark report
    // -------------------------------------------------------------------------
    MESSAGE("=== PerfBaselineTest Results ===");
    MESSAGE("  1. Sven stub load time (ParserPredefined)  : " << stubLoadMs << " ms");
    MESSAGE("  2. didOpen -> first publishDiagnostics     : " << didOpenToDiagnosticsMs << " ms");
    MESSAGE("  3. Debounced re-analysis after didChange   : " << debouncedDidChangeMs << " ms");
    MESSAGE("  4. GetSemanticTokens full                  : " << fullTokensMs << " ms (" << tokenCount << " tokens)");
    MESSAGE("  5. GetSemanticTokens delta                 : " << deltaTokensMs << " ms (" << editCount << " edits)");
    MESSAGE("  6. Peak resident memory                    : " << (peakResidentBytes / (1024.0 * 1024.0)) << " MB ("
                                                              << peakResidentBytes << " bytes)");

    CHECK(stubLoadMs > 0.0);
    CHECK(tokenCount > 0);
}
