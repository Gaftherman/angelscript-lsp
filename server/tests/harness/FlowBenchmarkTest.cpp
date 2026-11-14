/**
 * @file FlowBenchmarkTest.cpp
 * @brief Skipped-by-default latency and RSS memory benchmarks for core AngelLSP workflows.
 *
 * Measures parse/index/analysis ("open"), incremental edit ("didChange"), completion
 * (member, namespace scope, and empty prefix), hover, semantic tokens, document symbols,
 * workspace #include DAG scan/invalidation, and batched MCP session throughput.
 */

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/completion/CompletionTypes.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SemanticTokenFacts.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/TargetResolution.h"
#include "features/completion/CompletionHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/hover/HoverHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/LiveLspStream.h"
#include "helpers/McpTestSession.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "lsp/Server.h"
#include "lsp/mcp/McpJson.h"
#include "lsp/state/SyntaxRevisionCache.h"
#include "parser/AngelScriptParser.h"
#include "utils/IncludeResolver.h"
#include "utils/MemoryTelemetry.h"
#include "utils/Timer.h"
#include "utils/Utils.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <lsp/json/json.h>
#include <lsp/types.h>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace flow_bench
{

/**
 * @brief Fixed seed used by benchmark generators so every run is reproducible.
 */
inline constexpr uint64_t kBenchmarkSeed = 20261114ULL;

/**
 * @brief Summary statistics computed from a vector of latency samples in milliseconds.
 */
struct LatencySummary
{
    double p50Ms = 0.0; ///< Median latency (50th percentile, nearest-rank).
    double p90Ms = 0.0; ///< Tail latency (90th percentile, nearest-rank).
    double p95Ms = 0.0; ///< Tail latency (95th percentile, nearest-rank).
    double maxMs = 0.0; ///< Maximum observed latency.
};

/**
 * @brief Holds a generated AngelScript document and 0-based probe coordinates inside it.
 */
struct GeneratedScript
{
    std::string source;          ///< Complete generated AngelScript source text.
    size_t lineCount = 0;        ///< Total line count of the generated source.
    std::string memberName;      ///< Name of the public member on the probe class.
    std::string nsName;          ///< Name of the probe namespace.
    std::string nsMemberName;    ///< Name of the function declared inside the probe namespace.
    std::string hoverFuncName;   ///< Name of a generated function targeted by hover.
    uint32_t memberDotLine = 0;  ///< 0-based line positioned right after `obj.`.
    uint32_t memberDotCol = 0;   ///< 0-based column positioned right after `obj.`.
    uint32_t scopeColonLine = 0; ///< 0-based line positioned right after `Ns::`.
    uint32_t scopeColonCol = 0;  ///< 0-based column positioned right after `Ns::`.
    uint32_t emptyBodyLine = 0;  ///< 0-based line inside a function body with empty prefix.
    uint32_t emptyBodyCol = 0;   ///< 0-based column inside a function body with empty prefix.
    uint32_t hoverLine = 0;      ///< 0-based line of `hoverFuncName` declaration.
    uint32_t hoverCol = 0;       ///< 0-based column of `hoverFuncName` declaration.
    uint32_t editLine = 0;       ///< 0-based line near the middle suitable for insertion.
};

/**
 * @brief Holds the parsed AST and Layer 2 analysis artifacts for a single open document.
 */
struct OpenDocumentState
{
    angel_lsp::parser::AngelScriptParser parser;
    TSTree* tree = nullptr;
    angel_lsp::analysis::NodeIndex nodeIndex;
    angel_lsp::analysis::SymbolTable symbolTable;
    angel_lsp::analysis::ScopeIndex scopeIndex;
    std::shared_ptr<const angel_lsp::analysis::Scope> scopeRoot;
    angel_lsp::i18n::I18n i18n;
    size_t diagnosticCount = 0;
    double parseMs = 0.0;   ///< Last parse and NodeIndex build time.
    double scopeMs = 0.0;   ///< Last scope collection time.
    double symbolMs = 0.0;  ///< Last symbol collection time.
    double analyzeMs = 0.0; ///< Last semantic analysis time.

    OpenDocumentState() = default;
    OpenDocumentState(const OpenDocumentState&) = delete;
    OpenDocumentState& operator=(const OpenDocumentState&) = delete;

    ~OpenDocumentState()
    {
        if (tree != nullptr)
        {
            ts_tree_delete(tree);
            tree = nullptr;
        }
    }
};

/**
 * @brief RAII sandbox directory for temporary multi-file workspace benchmarks.
 */
struct BenchmarkSandbox
{
    std::filesystem::path rootDir;

    /**
     * @brief Creates a uniquely named temporary directory under the system temp folder.
     */
    BenchmarkSandbox()
    {
        const std::string name = angel_lsp::test::GenerateRandomSymbolName("flow_bench_sandbox");
        rootDir = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(rootDir);
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(rootDir, ec);
        if (!ec)
        {
            rootDir = std::move(canonical);
        }
    }

    /**
     * @brief Removes the temporary directory tree on scope exit.
     */
    ~BenchmarkSandbox()
    {
        std::error_code ec;
        std::filesystem::remove_all(rootDir, ec);
    }

    BenchmarkSandbox(const BenchmarkSandbox&) = delete;
    BenchmarkSandbox& operator=(const BenchmarkSandbox&) = delete;

    /**
     * @brief Writes a file relative to the sandbox root and returns its normalized path.
     * @param[in] relativePath Path relative to `rootDir`.
     * @param[in] content UTF-8 file contents to write.
     * @return Normalized filesystem path of the written file.
     */
    std::string WriteFile(const std::string& relativePath, const std::string& content) const
    {
        const std::filesystem::path fullPath = rootDir / relativePath;
        std::filesystem::create_directories(fullPath.parent_path());
        std::ofstream out(fullPath, std::ios::binary);
        out << content;
        return angel_lsp::utils::IncludeResolver::NormalizePath(fullPath);
    }
};

/**
 * @brief Computes nearest-rank p50, p90, p95, and max from a sample vector.
 * @param[in] samples Unsorted latency measurements in milliseconds.
 * @return Summary containing p50, p95, and max in milliseconds.
 */
inline LatencySummary ComputeLatencySummary(const std::vector<double>& samples)
{
    if (samples.empty())
    {
        return {};
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    const size_t n = sorted.size();
    auto nearestRank = [&sorted, n](size_t percentile) -> double
    {
        size_t rank = (n * percentile + 99) / 100;
        if (rank == 0)
        {
            rank = 1;
        }
        return sorted[rank - 1];
    };

    LatencySummary summary;
    summary.p50Ms = nearestRank(50);
    summary.p90Ms = nearestRank(90);
    summary.p95Ms = nearestRank(95);
    summary.maxMs = sorted.back();
    return summary;
}

/**
 * @brief Samples current OS process working set size in bytes.
 * @return Resident working set size in bytes.
 */
inline size_t ReadWorkingSetBytes() noexcept
{
    size_t workingSet = 0;
    size_t privateBytes = 0;
    angel_lsp::utils::GetProcessMemoryUsage(workingSet, privateBytes);
    return workingSet;
}

/**
 * @brief Computes signed working-set delta in mebibytes between two snapshots.
 * @param[in] beforeBytes Working set size before the measured operation.
 * @param[in] afterBytes Working set size after the measured operation.
 * @return Delta in MiB (`(after - before) / 1048576.0`).
 */
inline double ComputeRssDeltaMib(size_t beforeBytes, size_t afterBytes) noexcept
{
    constexpr double kBytesPerMib = 1024.0 * 1024.0;
    return (static_cast<double>(afterBytes) - static_cast<double>(beforeBytes)) / kBytesPerMib;
}

/**
 * @brief Emits the markdown table header once per benchmark TEST_CASE via doctest MESSAGE.
 */
inline void PrintTableHeader()
{
    MESSAGE("| flow | size | n | p50 ms | p90 ms | p95 ms | max ms | RSS delta MiB |");
    MESSAGE("| --- | --- | --- | --- | --- | --- | --- |");
}

/**
 * @brief Formats and prints a single markdown benchmark result row via doctest MESSAGE.
 * @param[in] flow Descriptive name of the measured workflow.
 * @param[in] sizeLabel Workload size label (e.g. "5000 lines", "400 files").
 * @param[in] samples Individual iteration latencies in milliseconds.
 * @param[in] rssDeltaMib Process working-set delta in MiB.
 */
inline void PrintTableRow(std::string_view flow, std::string_view sizeLabel, const std::vector<double>& samples,
                          double rssDeltaMib)
{
    const LatencySummary summary = ComputeLatencySummary(samples);
    std::ostringstream row;
    row << std::fixed << std::setprecision(3) << "| " << flow << " | " << sizeLabel << " | " << samples.size() << " | "
        << summary.p50Ms << " | " << summary.p90Ms << " | " << summary.p95Ms << " | " << summary.maxMs << " | "
        << std::setprecision(2) << rssDeltaMib << " |";
    MESSAGE(row.str());
}

/**
 * @brief Computes 0-based line and column for a byte offset in source text.
 * @param[in] source Full UTF-8 source buffer.
 * @param[in] offset Byte offset within `source`.
 * @return Pair of `{line, column}` (0-based).
 */
inline std::pair<uint32_t, uint32_t> LineColAtOffset(std::string_view source, size_t offset) noexcept
{
    const size_t clamped = std::min(offset, source.size());
    const uint32_t line =
        static_cast<uint32_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(clamped), '\n'));
    const size_t lastNewline = source.rfind('\n', clamped == 0 ? 0 : clamped - 1);
    const size_t lineStart = (lastNewline == std::string_view::npos) ? 0 : lastNewline + 1;
    const uint32_t col = static_cast<uint32_t>(clamped - lineStart);
    return {line, col};
}

/**
 * @brief Generates a synthetic AngelScript file of at least `targetLines` lines with completion and hover probes.
 * @param[in] targetLines Minimum number of lines to emit.
 * @param[in,out] rng Seeded Mersenne Twister engine for deterministic coefficients.
 * @return GeneratedScript containing the source text and 0-based probe positions.
 */
inline GeneratedScript GenerateBenchmarkScript(size_t targetLines, std::mt19937_64& rng)
{
    GeneratedScript result;
    result.memberName = angel_lsp::test::GenerateRandomSymbolName("member");
    result.nsName = angel_lsp::test::GenerateRandomSymbolName("Ns");
    result.nsMemberName = angel_lsp::test::GenerateRandomSymbolName("NsFn");
    const std::string probeClsName = angel_lsp::test::GenerateRandomSymbolName("ProbeCls");
    const std::string nsClsName = angel_lsp::test::GenerateRandomSymbolName("NsCls");

    std::string& code = result.source;
    code.reserve(targetLines * 52);

    code += "// Auto-generated flow benchmark script\n";
    code += "namespace " + result.nsName + "\n{\n";
    code += "    class " + nsClsName + "\n    {\n";
    code += "        int m_nsVal;\n";
    code += "        void Touch() {}\n";
    code += "    }\n\n";
    code += "    int " + result.nsMemberName + "(int x)\n    {\n";
    code += "        return x + 1;\n";
    code += "    }\n";
    code += "}\n\n";

    code += "class " + probeClsName + "\n{\n";
    code += "    int " + result.memberName + ";\n";
    code += "    float m_extra;\n";
    code += "    void Reset() { " + result.memberName + " = 0; }\n";
    code += "}\n\n";

    code += "namespace BenchmarkModule\n{\n";
    size_t currentLine = static_cast<size_t>(std::count(code.begin(), code.end(), '\n'));
    size_t classIdx = 0;
    std::uniform_int_distribution<int> coeffDist(2, 9);
    const size_t midpointLine = targetLines / 2;
    bool recordedEditLine = false;

    while (currentLine + 24 < targetLines)
    {
        const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Cls") + std::to_string(classIdx++);
        code += "    class " + clsName + "\n    {\n";
        code += "        int m_id;\n";
        code += "        float m_score;\n";
        code += "        bool m_active;\n\n";
        currentLine += 6;

        for (int m = 0; m < 10 && currentLine + 24 < targetLines; ++m)
        {
            const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("Compute") + std::to_string(m);
            if (result.hoverFuncName.empty())
            {
                result.hoverFuncName = fnName;
                const size_t fnOffset = code.size() + 12; // "        int "
                const auto [hLine, hCol] = LineColAtOffset(code + "        int " + fnName, fnOffset);
                result.hoverLine = hLine;
                result.hoverCol = hCol;
            }
            const int coeff = coeffDist(rng);
            code += "        int " + fnName + "(int a, float b)\n        {\n";
            code += "            int sum = a + " + std::to_string(coeff) + ";\n";
            code += "            float mult = b * 2.0f;\n";
            if (!recordedEditLine && currentLine >= midpointLine)
            {
                result.editLine = static_cast<uint32_t>(currentLine + 3);
                recordedEditLine = true;
            }
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

    code += "}\n\n";

    // Emit probe functions at the tail for member (.), namespace (::), and empty-prefix completion.
    const std::string memberProbeFn = angel_lsp::test::GenerateRandomSymbolName("ProbeMember");
    code += "void " + memberProbeFn + "()\n{\n";
    code += "    " + probeClsName + " obj;\n";
    code += "    obj.";
    const size_t dotOffset = code.size();
    code += "\n}\n\n";

    const std::string scopeProbeFn = angel_lsp::test::GenerateRandomSymbolName("ProbeScope");
    code += "void " + scopeProbeFn + "()\n{\n";
    code += "    " + result.nsName + "::";
    const size_t colonOffset = code.size();
    code += "\n}\n\n";

    const std::string emptyProbeFn = angel_lsp::test::GenerateRandomSymbolName("ProbeEmpty");
    code += "void " + emptyProbeFn + "(int paramVal)\n{\n";
    code += "    int localVal = paramVal + 1;\n";
    code += "    ";
    const size_t emptyOffset = code.size();
    code += "\n}\n";

    const auto [dotLine, dotCol] = LineColAtOffset(code, dotOffset);
    const auto [colonLine, colonCol] = LineColAtOffset(code, colonOffset);
    const auto [emptyLine, emptyCol] = LineColAtOffset(code, emptyOffset);
    result.memberDotLine = dotLine;
    result.memberDotCol = dotCol;
    result.scopeColonLine = colonLine;
    result.scopeColonCol = colonCol;
    result.emptyBodyLine = emptyLine;
    result.emptyBodyCol = emptyCol;
    if (!recordedEditLine)
    {
        result.editLine = static_cast<uint32_t>(currentLine / 2);
    }
    result.lineCount = static_cast<size_t>(std::count(code.begin(), code.end(), '\n') + 1);
    return result;
}

/**
 * @brief Executes the full open pipeline (Parse + NodeIndex + Scopes + Symbols + SemanticAnalyzer).
 * @param[in] uri Document URI passed to collectors and analyzer.
 * @param[in] source AngelScript source text to analyze.
 * @param[out] state Output state holding the parsed tree, index, symbol table, and scope tree.
 */
inline void RunOpenPipeline(std::string_view uri, std::string_view source, OpenDocumentState& state)
{
    if (state.tree != nullptr)
    {
        ts_tree_delete(state.tree);
        state.tree = nullptr;
    }

    angel_lsp::utils::HighResTimer phaseTimer;
    state.tree = state.parser.Parse(source);
    REQUIRE(state.tree != nullptr);

    const TSNode rootNode = ts_tree_root_node(state.tree);
    state.nodeIndex.Build(rootNode);
    state.parseMs = phaseTimer.ElapsedMs();
    phaseTimer = angel_lsp::utils::HighResTimer();

    angel_lsp::analysis::LocalScopeCollector scopeCollector(nullptr);
    state.scopeRoot = scopeCollector.CollectScopesFromIndex(state.nodeIndex, source);
    REQUIRE(state.scopeRoot != nullptr);
    state.scopeIndex.SetScopeTree(std::string(uri), state.scopeRoot);

    state.scopeMs = phaseTimer.ElapsedMs();
    phaseTimer = angel_lsp::utils::HighResTimer();
    state.symbolTable.ClearDocumentSymbols(std::string(uri));
    angel_lsp::analysis::SymbolCollector symbolCollector(nullptr);
    symbolCollector.CollectSymbolsWithIndex(angel_lsp::analysis::SymbolCollectRequest(std::string(uri), source),
                                            state.nodeIndex, state.symbolTable);

    state.symbolMs = phaseTimer.ElapsedMs();
    phaseTimer = angel_lsp::utils::HighResTimer();
    angel_lsp::analysis::SemanticAnalysisRequest semReq(state.symbolTable, std::string(uri), "", &state.i18n);
    semReq.sourceCode = source;
    semReq.scopeRoot = state.scopeRoot;
    semReq.nodeIndex = &state.nodeIndex;

    angel_lsp::analysis::SemanticAnalyzer analyzer(nullptr);
    const auto diagnostics = analyzer.Analyze(semReq);
    state.analyzeMs = phaseTimer.ElapsedMs();
    state.diagnosticCount = diagnostics.size();
}

/**
 * @brief Inserts a single valid statement line at `insertAfterLine` (0-based).
 * @param[in] source Original source text.
 * @param[in] insertAfterLine 0-based line index after which to insert.
 * @param[in] lineText Line contents without trailing newline.
 * @return Modified source text containing the inserted line.
 */
inline std::string InsertLineAt(std::string_view source, uint32_t insertAfterLine, std::string_view lineText)
{
    size_t pos = 0;
    uint32_t line = 0;
    while (pos < source.size() && line <= insertAfterLine)
    {
        const size_t nextNl = source.find('\n', pos);
        if (nextNl == std::string_view::npos)
        {
            pos = source.size();
            break;
        }
        pos = nextNl + 1;
        ++line;
    }

    std::string modified;
    modified.reserve(source.size() + lineText.size() + 2);
    modified.append(source.substr(0, pos));
    modified.append(lineText);
    modified.push_back('\n');
    modified.append(source.substr(pos));
    return modified;
}

/**
 * @brief Creates a multi-file project with chained and diamond #include edges inside `sandbox`.
 * @param[in] sandbox Temporary directory guard owning the generated files.
 * @param[in] fileCount Total number of `.as` files to write.
 * @param[in,out] rng Seeded Mersenne Twister engine for diamond cross-edges.
 * @return Normalized file paths of all generated `.as` files (index 0 is the shared root).
 */
inline std::vector<std::string> CreateIncludeProject(const BenchmarkSandbox& sandbox, size_t fileCount,
                                                     std::mt19937_64& rng)
{
    std::vector<std::string> files;
    files.reserve(fileCount);

    const std::string sharedContent = "class CoreShared\n{\n    int version;\n    void Init() {}\n}\n";
    files.push_back(sandbox.WriteFile("CoreShared.as", sharedContent));

    for (size_t i = 1; i < fileCount; ++i)
    {
        const std::string fileName = "Module_" + std::to_string(i) + ".as";
        std::string content = "#include \"CoreShared.as\"\n";
        if (i > 1)
        {
            content += "#include \"Module_" + std::to_string(i - 1) + ".as\"\n";
        }
        if (i >= 4)
        {
            std::uniform_int_distribution<size_t> dist(1, i - 2);
            const size_t cross = dist(rng);
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

/**
 * @brief Builds `{path, line, character, limit}` JSON arguments for MCP completion.
 * @param[in] path Relative path inside the MCP session root.
 * @param[in] line 0-based cursor line.
 * @param[in] character 0-based cursor column.
 * @param[in] limit Maximum number of completion items to return.
 * @return JSON object ready for `McpTestSession::Call("completion", ...)`.
 */
inline lsp::json::Value MakeMcpCompletionArgs(const std::string& path, int line, int character, int limit)
{
    lsp::json::Value args = angel_lsp::test::McpPositionArgs(path, line, character);
    args.object()["limit"] = limit;
    return args;
}

/**
 * @brief Builds `{path, startLine, startCharacter, endLine, endCharacter, text}` for MCP `apply_edit`.
 * @param[in] path Relative path inside the MCP session root.
 * @param[in] line 0-based line where text is replaced/inserted.
 * @param[in] text Replacement text for the single-point edit.
 * @return JSON object ready for `McpTestSession::Call("apply_edit", ...)`.
 */
inline lsp::json::Value MakeMcpPointEditArgs(const std::string& path, int line, const std::string& text)
{
    lsp::json::Value args = angel_lsp::test::McpPathArgs(path);
    args.object()["startLine"] = line;
    args.object()["startCharacter"] = 0;
    args.object()["endLine"] = line;
    args.object()["endCharacter"] = 0;
    args.object()["text"] = std::string(text);
    return args;
}

} // namespace flow_bench

TEST_SUITE("harness/FlowBenchmark")
{
    TEST_CASE("FlowBenchmark - Open Pipeline (1k, 5k, 20k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        struct SizeSpec
        {
            size_t targetLines;
            size_t iterations;
            const char* label;
        };
        const SizeSpec specs[] = {
            {1000, 10, "1000 lines"},
            {5000, 10, "5000 lines"},
            {20000, 5, "20000 lines"},
        };

        for (const SizeSpec& spec : specs)
        {
            const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(spec.targetLines, rng);
            REQUIRE(script.lineCount >= spec.targetLines);
            const std::string docUri = "file:///bench_open_" + std::to_string(spec.targetLines) + ".as";

            std::vector<double> samples;
            samples.reserve(spec.iterations);
            const size_t rssBefore = flow_bench::ReadWorkingSetBytes();
            size_t symbolCount = 0;

            for (size_t iter = 0; iter < spec.iterations; ++iter)
            {
                flow_bench::OpenDocumentState state;
                angel_lsp::utils::HighResTimer timer;
                flow_bench::RunOpenPipeline(docUri, script.source, state);
                samples.push_back(timer.ElapsedMs());
                symbolCount = state.symbolTable.GetAllSymbols().size();
                if (iter + 1 == spec.iterations)
                {
                    MESSAGE("phases " << spec.label << " | parse+index " << state.parseMs << " | scopes "
                                      << state.scopeMs << " | symbols " << state.symbolMs << " | analyze "
                                      << state.analyzeMs);
                }
            }

            const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
            CHECK(symbolCount > 0);
            flow_bench::PrintTableRow("open (parse+index+scope+sym+analyze)", spec.label, samples,
                                      flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
        }
    }

    TEST_CASE("FlowBenchmark - Edit Path (didChange with diagnostics, 5k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(5000, rng);
        REQUIRE(script.lineCount >= 5000);
        const std::string docUri = "file:///bench_edit_5000.as";

        flow_bench::OpenDocumentState state;
        flow_bench::RunOpenPipeline(docUri, script.source, state);
        REQUIRE(state.symbolTable.GetAllSymbols().size() > 0);

        constexpr size_t kIterations = 20;
        std::vector<double> samples;
        samples.reserve(kIterations);
        const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

        for (size_t iter = 0; iter < kIterations; ++iter)
        {
            const std::string inserted = "            int editProbe_" + std::to_string(iter) + " = 42;";
            const std::string editedSource = flow_bench::InsertLineAt(script.source, script.editLine, inserted);

            angel_lsp::utils::HighResTimer timer;
            flow_bench::RunOpenPipeline(docUri, editedSource, state);
            samples.push_back(timer.ElapsedMs());
        }

        const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
        CHECK(state.symbolTable.GetAllSymbols().size() > 0);
        flow_bench::PrintTableRow("didChange with diagnostics", "5000 lines", samples,
                                  flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
    }

    TEST_CASE("FlowBenchmark - Completion (member, scope, empty prefix on 5k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(5000, rng);
        REQUIRE(script.lineCount >= 5000);
        const std::string docUri = "file:///bench_completion_5000.as";

        flow_bench::OpenDocumentState state;
        flow_bench::RunOpenPipeline(docUri, script.source, state);

        struct CompletionCase
        {
            const char* label;
            uint32_t line;
            uint32_t col;
        };
        const CompletionCase cases[] = {
            {"completion (member obj.)", script.memberDotLine, script.memberDotCol},
            {"completion (scope Ns::)", script.scopeColonLine, script.scopeColonCol},
            {"completion (empty prefix)", script.emptyBodyLine, script.emptyBodyCol},
        };

        constexpr size_t kIterations = 50;
        for (const CompletionCase& probe : cases)
        {
            const angel_lsp::analysis::completion::CompletionQuery query{
                docUri,           script.source,
                &state.nodeIndex, state.symbolTable,
                state.scopeIndex, angel_lsp::analysis::TargetPosition{probe.line, probe.col}};

            std::vector<double> samples;
            samples.reserve(kIterations);
            size_t lastCount = 0;
            const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

            for (size_t iter = 0; iter < kIterations; ++iter)
            {
                angel_lsp::utils::HighResTimer timer;
                const std::vector<lsp::CompletionItem> items = angel_lsp::features::GetCompletion(query);
                samples.push_back(timer.ElapsedMs());
                lastCount = items.size();
            }

            const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
            CHECK(lastCount > 0);
            flow_bench::PrintTableRow(probe.label, "5000 lines", samples,
                                      flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
        }
    }

    TEST_CASE("FlowBenchmark - Hover, Semantic Tokens, Document Symbols (5k & 20k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        const flow_bench::GeneratedScript script5k = flow_bench::GenerateBenchmarkScript(5000, rng);
        const flow_bench::GeneratedScript script20k = flow_bench::GenerateBenchmarkScript(20000, rng);
        REQUIRE(script5k.lineCount >= 5000);
        REQUIRE(script20k.lineCount >= 20000);

        constexpr size_t kIterations = 50;

        // 5000-line file: Hover, SemanticTokens, DocumentSymbols
        {
            const std::string docUri = "file:///bench_features_5000.as";
            flow_bench::OpenDocumentState state;
            flow_bench::RunOpenPipeline(docUri, script5k.source, state);

            // Hover
            {
                const angel_lsp::features::HoverRequest hoverReq{
                    docUri,           script5k.source,
                    &state.nodeIndex, state.symbolTable,
                    state.scopeIndex, lsp::Position{script5k.hoverLine, script5k.hoverCol}};

                std::vector<double> samples;
                samples.reserve(kIterations);
                bool hasHover = false;
                const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

                for (size_t iter = 0; iter < kIterations; ++iter)
                {
                    angel_lsp::utils::HighResTimer timer;
                    const auto res = angel_lsp::features::GetHover(hoverReq);
                    samples.push_back(timer.ElapsedMs());
                    hasHover = res.has_value();
                }

                const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
                CHECK(hasHover);
                flow_bench::PrintTableRow("hover", "5000 lines", samples,
                                          flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
            }

            // Semantic tokens
            {
                const auto semFacts = angel_lsp::analysis::CollectSemanticTokenFacts(state.nodeIndex, script5k.source);
                angel_lsp::features::SemanticTokensRequest tokReq(docUri, script5k.source, semFacts, state.symbolTable);
                tokReq.scopeRoot = state.scopeRoot;

                std::vector<double> samples;
                samples.reserve(kIterations);
                size_t tokenDataSize = 0;
                const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

                for (size_t iter = 0; iter < kIterations; ++iter)
                {
                    angel_lsp::utils::HighResTimer timer;
                    const lsp::SemanticTokens tokens = angel_lsp::features::GetSemanticTokens(tokReq);
                    samples.push_back(timer.ElapsedMs());
                    tokenDataSize = tokens.data.size();
                }

                const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
                CHECK(tokenDataSize > 0);
                flow_bench::PrintTableRow("semantic tokens", "5000 lines", samples,
                                          flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
            }

            // Document symbols
            {
                const angel_lsp::features::DocumentSymbolRequest symReq{docUri, script5k.source, state.symbolTable};

                std::vector<double> samples;
                samples.reserve(kIterations);
                size_t symCount = 0;
                const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

                for (size_t iter = 0; iter < kIterations; ++iter)
                {
                    angel_lsp::utils::HighResTimer timer;
                    const auto syms = angel_lsp::features::GetDocumentSymbols(symReq);
                    samples.push_back(timer.ElapsedMs());
                    symCount = syms.has_value() ? syms->size() : 0;
                }

                const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
                CHECK(symCount > 0);
                flow_bench::PrintTableRow("document symbols", "5000 lines", samples,
                                          flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
            }
        }

        // 20000-line file: SemanticTokens, DocumentSymbols
        {
            const std::string docUri = "file:///bench_features_20000.as";
            flow_bench::OpenDocumentState state;
            flow_bench::RunOpenPipeline(docUri, script20k.source, state);

            // Semantic tokens
            {
                const auto semFacts = angel_lsp::analysis::CollectSemanticTokenFacts(state.nodeIndex, script20k.source);
                angel_lsp::features::SemanticTokensRequest tokReq(docUri, script20k.source, semFacts,
                                                                  state.symbolTable);
                tokReq.scopeRoot = state.scopeRoot;

                std::vector<double> samples;
                samples.reserve(kIterations);
                size_t tokenDataSize = 0;
                const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

                for (size_t iter = 0; iter < kIterations; ++iter)
                {
                    angel_lsp::utils::HighResTimer timer;
                    const lsp::SemanticTokens tokens = angel_lsp::features::GetSemanticTokens(tokReq);
                    samples.push_back(timer.ElapsedMs());
                    tokenDataSize = tokens.data.size();
                }

                const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
                CHECK(tokenDataSize > 0);
                flow_bench::PrintTableRow("semantic tokens", "20000 lines", samples,
                                          flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
            }

            // Document symbols
            {
                const angel_lsp::features::DocumentSymbolRequest symReq{docUri, script20k.source, state.symbolTable};

                std::vector<double> samples;
                samples.reserve(kIterations);
                size_t symCount = 0;
                const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

                for (size_t iter = 0; iter < kIterations; ++iter)
                {
                    angel_lsp::utils::HighResTimer timer;
                    const auto syms = angel_lsp::features::GetDocumentSymbols(symReq);
                    samples.push_back(timer.ElapsedMs());
                    symCount = syms.has_value() ? syms->size() : 0;
                }

                const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
                CHECK(symCount > 0);
                flow_bench::PrintTableRow("document symbols", "20000 lines", samples,
                                          flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
            }
        }
    }

    TEST_CASE("FlowBenchmark - Workspace Include DAG (400 files)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        flow_bench::BenchmarkSandbox sandbox;
        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        constexpr size_t kFileCount = 400;
        const std::vector<std::string> files = flow_bench::CreateIncludeProject(sandbox, kFileCount, rng);
        REQUIRE(files.size() == kFileCount);

        angel_lsp::utils::WorkspaceIncludeGraph graph;
        const size_t scanRssBefore = flow_bench::ReadWorkingSetBytes();
        angel_lsp::utils::HighResTimer scanTimer;
        graph.BuildFromFiles(files, {sandbox.rootDir.string()}, {sandbox.rootDir.string()});
        const double scanMs = scanTimer.ElapsedMs();
        const size_t scanRssAfter = flow_bench::ReadWorkingSetBytes();

        CHECK(graph.FileCount() == kFileCount);
        flow_bench::PrintTableRow("include-graph scan", "400 files", {scanMs},
                                  flow_bench::ComputeRssDeltaMib(scanRssBefore, scanRssAfter));

        const size_t invRssBefore = flow_bench::ReadWorkingSetBytes();
        angel_lsp::utils::HighResTimer invTimer;
        const std::vector<std::string> affected = graph.GetReverseDependenciesTopological(files.front());
        const double invMs = invTimer.ElapsedMs();
        const size_t invRssAfter = flow_bench::ReadWorkingSetBytes();

        CHECK(affected.size() >= kFileCount - 1);
        flow_bench::PrintTableRow("include-graph invalidation", "400 files", {invMs},
                                  flow_bench::ComputeRssDeltaMib(invRssBefore, invRssAfter));
    }

    TEST_CASE("FlowBenchmark - End-to-End MCP Session (5k lines, 221 requests)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(5000, rng);
        REQUIRE(script.lineCount >= 5000);

        angel_lsp::test::McpTestSession session;
        const std::string relPath = "bench_mcp.as";
        std::vector<int> requestIds;
        requestIds.reserve(221);

        requestIds.push_back(session.Call("open_document", angel_lsp::test::McpArgs(relPath, script.source)));

        for (int i = 0; i < 100; ++i)
        {
            requestIds.push_back(
                session.Call("hover", angel_lsp::test::McpPositionArgs(relPath, static_cast<int>(script.hoverLine),
                                                                       static_cast<int>(script.hoverCol))));
        }

        for (int i = 0; i < 100; ++i)
        {
            requestIds.push_back(session.Call(
                "completion", flow_bench::MakeMcpCompletionArgs(relPath, static_cast<int>(script.memberDotLine),
                                                                static_cast<int>(script.memberDotCol), 50)));
        }

        for (int i = 0; i < 20; ++i)
        {
            const std::string editText = "// mcp_edit_" + std::to_string(i) + "\n";
            requestIds.push_back(session.Call(
                "apply_edit", flow_bench::MakeMcpPointEditArgs(relPath, static_cast<int>(script.editLine), editText)));
        }

        const size_t rssBefore = flow_bench::ReadWorkingSetBytes();
        angel_lsp::utils::HighResTimer sessionTimer;
        const std::map<int, lsp::json::Value> replies = session.Run();
        const double totalMs = sessionTimer.ElapsedMs();
        const size_t rssAfter = flow_bench::ReadWorkingSetBytes();

        REQUIRE(replies.size() == requestIds.size());
        for (const int id : requestIds)
        {
            REQUIRE(replies.contains(id));
            angel_lsp::test::McpPayload(replies.at(id), false);
        }

        const double avgPerRequestMs = totalMs / static_cast<double>(requestIds.size());
        flow_bench::PrintTableRow("mcp batch avg (open+100hover+100comp+20edit)", "5000 lines", {avgPerRequestMs},
                                  flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
    }

    TEST_CASE("FlowBenchmark - Syntax Revision Re-request (20k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(20000, rng);
        REQUIRE(script.lineCount >= 20000);
        const std::string docUri = "file:///bench_syntax_rerequest_20000.as";

        angel_lsp::parser::AngelScriptParser parser;
        angel_lsp::SyntaxRevisionCache cache;
        const angel_lsp::SyntaxRevisionKey key{docUri, script.source, /*generation=*/1};

        // Warmup call populates the cache for the 20k-line open revision.
        const angel_lsp::SyntaxRevision warmup =
            angel_lsp::ResolveSyntaxRevision(angel_lsp::SyntaxRevisionInput{.key = key, .parser = parser}, &cache);
        REQUIRE(warmup.syntax != nullptr);
        REQUIRE(warmup.extractedNodes > 32768);

        constexpr size_t kIterations = 20;
        std::vector<double> samples;
        samples.reserve(kIterations);
        size_t totalExtractedNodes = 0;
        std::shared_ptr<const angel_lsp::ParsedDocumentSyntax> lastSyntax;
        const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

        for (size_t iter = 0; iter < kIterations; ++iter)
        {
            angel_lsp::utils::HighResTimer timer;
            const angel_lsp::SyntaxRevision rev =
                angel_lsp::ResolveSyntaxRevision(angel_lsp::SyntaxRevisionInput{.key = key, .parser = parser}, &cache);
            samples.push_back(timer.ElapsedMs());
            totalExtractedNodes += rev.extractedNodes;
            lastSyntax = rev.syntax;
        }

        const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
        CHECK(lastSyntax == warmup.syntax);
        CHECK(totalExtractedNodes == 0);
        flow_bench::PrintTableRow("syntax revision re-request", "20000 lines", samples,
                                  flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
    }

    TEST_CASE("FlowBenchmark - Burst of 10 didChange to final diagnostics (5k, 20k lines)" * doctest::skip(true))
    {
        MESSAGE("Seed: " << flow_bench::kBenchmarkSeed);
        flow_bench::PrintTableHeader();

        std::mt19937_64 rng(flow_bench::kBenchmarkSeed);
        struct BurstSpec
        {
            size_t targetLines;
            size_t bursts;
            const char* label;
        };
        const BurstSpec specs[] = {
            {5000, 10, "5000 lines"},
            {20000, 5, "20000 lines"},
        };

        for (const BurstSpec& spec : specs)
        {
            const flow_bench::GeneratedScript script = flow_bench::GenerateBenchmarkScript(spec.targetLines, rng);
            REQUIRE(script.lineCount >= spec.targetLines);

            const flow_bench::BenchmarkSandbox sandbox;
            const std::string docPath = sandbox.WriteFile("burst_bench.as", script.source);
            const std::string docUri = angel_lsp::utils::PathToUri(docPath);
            const std::string rootUri = angel_lsp::utils::PathToUri(sandbox.rootDir.string());

            angel_lsp::config::ServerConfig serverConfig;
            angel_lsp::test::LiveLspStream stream;
            angel_lsp::Server server(serverConfig, stream);
            std::thread serverThread([&server]() { server.Run(); });

            int nextId = 1;
            stream.PushJson(angel_lsp::test::MakeInitializeMessage(nextId, rootUri));
            stream.WaitForResponseId(nextId++);
            stream.PushJson(
                angel_lsp::test::MakeNotificationMessage("initialized", lsp::json::Value(lsp::json::Object{})));

            int docVersion = 1;
            stream.PushJson(angel_lsp::test::MakeDidOpenMessage(docUri, script.source, docVersion));
            stream.WaitForPublishedVersion(docVersion);

            std::vector<double> samples;
            samples.reserve(spec.bursts);
            const size_t rssBefore = flow_bench::ReadWorkingSetBytes();

            for (size_t burst = 0; burst < spec.bursts; ++burst)
            {
                constexpr int kBurstSize = 10;
                const int finalVersion = docVersion + kBurstSize;
                angel_lsp::utils::HighResTimer timer;
                for (int step = 0; step < kBurstSize; ++step)
                {
                    ++docVersion;
                    const std::string inserted =
                        "            int burstProbe_" + std::to_string(burst) + "_" + std::to_string(step) + " = 7;";
                    const std::string editedSource = flow_bench::InsertLineAt(script.source, script.editLine, inserted);
                    stream.PushJson(angel_lsp::test::MakeDidChangeWholeMessage(docUri, docVersion, editedSource));
                }
                stream.WaitForPublishedVersion(finalVersion);
                samples.push_back(timer.ElapsedMs());
            }

            const size_t rssAfter = flow_bench::ReadWorkingSetBytes();
            stream.PushJson(angel_lsp::test::MakeShutdownMessage(nextId));
            stream.WaitForResponseId(nextId++);
            stream.PushJson(angel_lsp::test::MakeNotificationMessage("exit", lsp::json::Value{}));
            stream.Close();
            serverThread.join();

            flow_bench::PrintTableRow("burst 10 didChange to final diags", spec.label, samples,
                                      flow_bench::ComputeRssDeltaMib(rssBefore, rssAfter));
        }
    }
}
