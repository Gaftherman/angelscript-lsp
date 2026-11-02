#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/VirtualMixinContext.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "utils/LspLogger.h"
#include "utils/Timer.h"

#include <functional>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a hover request.
 */
struct HoverRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    lsp::Position position;

    /**
     * @brief Reads a document's text by URI, or returns nullptr when the server holds none.
     *        A documentation comment lives above the *declaration*, which is often in another file,
     */
    std::function<const std::string*(const std::string&)> readDocument = {};

    /**
     * @brief The server's configuration, for the engine properties that change what is legal.
     *        asEP_PROPERTY_ACCESSOR_MODE above all: whether `get_X` is the property `X` is the host's
     */
    const config::ServerConfig* config = nullptr;

    /**
     * @brief Resolves the text inside an `#include` to an absolute path, or "" when it misses.
     *        Supplied by the caller rather than done here, for the reason readDocument is: resolution
     */
    std::function<std::string(const std::string& rawPath)> resolveInclude = {};

    /**
     * @brief Optional LSP logger for performance profiling telemetry.
     */
    angel_lsp::utils::LspLogger* logger = nullptr;

    /**
     * @brief Optional localization instance for translating UI feedback and overload summaries.
     */
    const angel_lsp::i18n::I18n* i18n = nullptr;
};

/**
 * @brief Computes hover tooltip information for symbol under cursor.
 * @param request Immutable context for hover computation.
 * @return Optional Hover object, nullopt if no symbol found.
 */
std::optional<lsp::Hover> GetHover(const HoverRequest& request);

/**
 * @brief Resolved symbol descriptor and declaration node target for hover presentation.
 */
struct HoverTarget
{
    TSNode node = {};
    std::string text;
    lsp::Range range{};
};

/**
 * @brief Parsed include directive metadata containing line index, file path, and angled bracket delimiter flag.
 */
struct IncludeDirectiveInfo
{
    size_t hash = 0;
    size_t closeQuote = 0;
    std::string rawPath;
};

/**
 * @brief Trace profiler recording millisecond latency across hover analysis stages.
 */
struct HoverProfiler
{
    utils::HighResTimer totalTimer;
    angel_lsp::utils::LspLogger* m_logger = nullptr;
    uint32_t line = 0;
    uint32_t character = 0;
    std::string_view nodeType = "";
    std::string pathText;
    std::string symbolName;
    double nodeMs = 0.0;
    double symMs = 0.0;
    double fmtMs = 0.0;
    bool emitted = false;

    [[nodiscard]] bool IsActive() const noexcept;
    void Emit();
    ~HoverProfiler();
};

/**
 * @brief Parameters and AST node descriptors for evaluating hover tooltips at a document position.
 */
struct HoverQueryContext
{
    const HoverRequest& request;
    HoverProfiler& profiler;
    TSNode node = {};
    std::string nodeText;
    lsp::Range range{};
    analysis::VirtualMixinContext vctx;
    std::shared_ptr<const analysis::Scope> rootScope;
    uint32_t queryLine = 0;
    const analysis::Scope* scope = nullptr;
    TSNode parent = {};
};

/**
 * @brief Preference ranking for resolving symbols within enclosing container scopes versus imported scopes.
 */
enum class ContainerContextPreference
{
    None,
    PreferClass,
    PreferNamespace
};
} // namespace angel_lsp::features
