#pragma once

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/CallGraph.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "document/Document.h"
#include "lsp/state/PositionCodec.h"
#include "lsp/state/SyntaxRevisionCache.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"
#include "utils/LspLogger.h"
#include "utils/PreprocessorRegions.h"
#include "utils/StopFlag.h"
#include "utils/Timer.h"
#include "utils/TransparentHash.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <lsp/types.h>
#include <memory>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp
{

/**
 * @brief Parameters for resolving valid breakpoint positions within a source line range.
 */
struct BreakpointLocationsParams
{
    std::string uri;
    uint32_t startLine = 0;
    uint32_t endLine = std::numeric_limits<uint32_t>::max();
};

/**
 * @brief Identifies virtual mixin methods and their backing source declarations for LSP navigation.
 */
struct VirtualMixinTarget
{
    std::string hostClass;
    std::string mixinName;
};

/**
 * @brief Source location and document URI of a symbol defined within an expanded mixin block.
 */
struct MixinSymbolLocation
{
    std::string fileUri;
    uint32_t startLine{0};
    uint32_t endLine{0};
    std::string resolvedBaseClass;
    bool found{false};
};

/**
 * @brief Snapshot of computed semantic token data and revision tracking for delta computations.
 */
struct SemanticTokensSnapshot
{
    std::string resultId;
    std::vector<lsp::uint> data;
    bool hasError = false;
    int version = -1;
};

/**
 * @brief Thread-safe snapshot of cached semantic tokens and version metadata for document synchronization.
 */
struct CachedTokensSnapshot
{
    bool canDiff{false};
    bool prevHadError{false};
    int cachedVersion{-1};
    std::vector<lsp::uint> previous;
};

/**
 * @brief RAII timer recording execution duration for LSP server startup phases.
 */
class PhaseTimer
{
  public:
    PhaseTimer(angel_lsp::utils::LspLogger* logger, std::string_view phase)
        : m_logger(logger), m_phase(phase), m_start(std::chrono::steady_clock::now())
    {
    }

    ~PhaseTimer()
    {
        if (m_logger == nullptr)
        {
            return;
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start);
        m_logger->LogInfo(fmt::format("Startup phase '{}': {}", m_phase,
                                      angel_lsp::utils::FormatDuration(static_cast<double>(elapsed.count()))));
    }

    PhaseTimer(const PhaseTimer&) = delete;
    PhaseTimer& operator=(const PhaseTimer&) = delete;

  private:
    angel_lsp::utils::LspLogger* m_logger;
    std::string m_phase;
    std::chrono::steady_clock::time_point m_start;
};

/**
 * @brief Associates a diagnostic code with its user-configured severity setting.
 */
struct CodeSettingPair
{
    std::string_view code;
    std::string_view uri;
};

/**
 * @brief RAII guard re-triggering workspace diagnostic analysis upon background task termination.
 */
struct ReanalyseOnExit
{
    std::function<void()> onExit;
    ~ReanalyseOnExit()
    {
        if (onExit)
        {
            onExit();
        }
    }
};

/**
 * @brief Context parameters for recursive predefined stub loading and include parsing.
 */
struct PredefinedLoadContext
{
    angel_lsp::parser::AngelScriptParser& parser;
    bool forceReload = false;
    std::unordered_set<std::string>& visited;
};

/**
 * @brief Detailed timing breakdown for indexing a predefined stub.
 */
struct PredefinedIndexTiming
{
    double parseMs = 0.0;
    double symMs = 0.0;
    double scopeMs = 0.0;
};

/// Source passed to the parser and the directive scan that produced it.
struct PreparedAnalysisSource
{
    std::string text;
    std::shared_ptr<const utils::PreprocessorScan> preprocessing;
};

/**
 * @brief Parameters required to atomically commit analysis results for a document.
 */
struct CommitAnalysisRequest
{
    std::string uriStr;
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;
    analysis::SymbolTable* staging = nullptr;
    std::shared_ptr<const analysis::Scope> scopeRoot;
    std::vector<analysis::CallSite> calls;
    std::vector<analysis::Diagnostic> diagnostics;
    std::string text;
    uint64_t closedRevision = 0;
    std::shared_ptr<const utils::PreprocessorScan> preprocessing = {};
};

/**
 * @brief Parameters for scope collection and semantic analysis invocation.
 */
struct CollectScopesRequest
{
    std::string uriStr;
    const PreparedAnalysisSource& source;
    bool hasTree = false;
    double* outScopeMs = nullptr;
    double* outCheckMs = nullptr;
    const analysis::NodeIndex& nodeIndex;
    std::optional<analysis::CallBindingRevision> callBindingRevision = std::nullopt;
};

/**
 * @brief Request bundle for publishing diagnostics for a document.
 */
struct PublishDiagnosticsRequest
{
    std::string uriStr;
    std::string text;
    std::vector<angel_lsp::analysis::Diagnostic> diagnostics;
    int version = -1;
    uint64_t generation = 0;
    std::shared_ptr<const utils::PreprocessorScan> preprocessing = {};
    uint64_t closedRevision = 0;
};

/**
 * @brief Bundles parameters for processing an open predefined stub file.
 */
struct DidOpenPredefinedRequest
{
    std::string uriStr;
    std::string text;
    int version = 0;
    std::string clientUri;
    const utils::HighResTimer& totalTimer;
};

/**
 * @brief Bundles discovered file collections from a unified workspace filesystem walk.
 */
struct WorkspaceFilesWalkResult
{
    std::vector<std::string> allScriptFiles;
    std::vector<std::string> allFileNames;
    std::vector<std::string> discoveredStubPaths;
};

/**
 * @brief Request bundle for queueing background or immediate document analysis.
 */
struct ScheduleAnalysisRequest
{
    std::string uriStr;
    std::string text;
    bool force = false;
    int version = -1;
    uint64_t generation = 0;
    std::optional<PreparedAnalysisSource> preparedSource = {};
};

/**
 * @brief Request bundle for analyzing a document in the worker thread.
 */
struct AnalyzeDocumentRequest
{
    std::string uriStr;
    std::string text;
    angel_lsp::parser::AngelScriptParser& parser;
    angel_lsp::parser::TreePtr treeCopy = angel_lsp::parser::MakeTreePtr(nullptr);
    int version = -1;
    uint64_t generation = 0;
    uint64_t configRevision = 0;
    uint64_t closedRevision = 0;
    bool fromScheduler = false;
    std::shared_ptr<const utils::PreprocessorScan> preprocessing = {};
};

/// Value-only syntax results published for a closed document's exact parser input.
struct ClosedIndexedSyntax
{
    std::shared_ptr<const std::string> source;
    std::vector<analysis::Diagnostic> diagnostics;
};

/// Closed analysis can borrow immutable lexical records and use the published symbols.
struct ClosedAnalysisInputs
{
    std::shared_ptr<const ClosedIndexedSyntax> indexedSyntax;
    std::shared_ptr<const analysis::Scope> lexicalScopes;
};

/**
 * @brief Per-request syntax diagnostics and timings, including whether a cursor ran.
 */
struct ParseAndCollectResult
{
    std::shared_ptr<const ParsedDocumentSyntax> syntax;
    std::vector<angel_lsp::analysis::Diagnostic> diagnostics;
    double parseMs = 0.0;
    double colMs = 0.0;
    size_t extractedNodes = 0;
    ClosedAnalysisInputs closed = {};
};

/**
 * @brief Aggregated phase timings for document analysis telemetry.
 */
struct AnalysisTimingProfile
{
    double totalMs = 0.0;
    double snapshotMs = 0.0;
    double parseMs = 0.0;
    double colMs = 0.0;
    double scopeMs = 0.0;
    double checkMs = 0.0;
    size_t extractedNodes = 0;
};

/**
 * @brief The three components resolved for nearly every document request.
 */
struct OpenDocument
{
    std::string uri;
    const std::string* text = nullptr;
    std::shared_ptr<const document::Document> docHandle;
    std::shared_ptr<const std::string> predefinedHandle;
};

/**
 * @brief Context bundle for prepare-rename and rename operations.
 */
struct RenameContext
{
    OpenDocument doc;
    TransparentStringSet predefinedUris;
    lsp::Position decodedPosition;
};

} // namespace angel_lsp
