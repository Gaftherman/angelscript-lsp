#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include <functional>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
struct TemplateBinding;
}

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for an auto-completion request.
 */
struct CompletionRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    lsp::Position position;
    const config::ServerConfig* config = nullptr;

    /**
     * @brief Whether the client renders `${1:T}` placeholders rather than printing them.
     *
     * Read from `textDocument.completion.completionItem.snippetSupport` at initialize. It has
     * to be asked rather than assumed: a template class completes to `array<${1:T}>`, and a
     * client without snippet support would insert those six characters literally.
     */
    bool snippetSupport = false;

    /**
     * @brief Absolute path of the document being completed, for relative include paths.
     *
     * An include is written relative to the file it sits in, so the answer to "which files can
     * I include" is different in every directory. Empty disables include completion, which is
     * what a caller with no filesystem gets.
     */
    std::string documentPath = {};

    /**
     * @brief Every file that may be named in an `#include`, as absolute paths.
     *
     * A callback rather than a list, because the answer costs a walk of the include graph and
     * almost no completion request is inside an include. Supplied by the server, which is the
     * only thing that knows what the workspace holds.
     */
    std::function<std::vector<std::string>()> listIncludeCandidates = {};

    /**
     * @brief Suffix to leave off an inserted include path, or empty to insert the name in full.
     *
     * When the host resolves the extension itself the correct spelling is the short one -
     * `#include "helper"`, not `#include "helper.as"` - so completing to the full filename
     * would insert something the host cannot open. See ServerConfig::implicitIncludeExtension.
     */
    std::string implicitExtension = {};

    /**
     * @brief Optional callback returning module symbols matching a prefix as (name, detail) pairs.
     */
    std::function<std::vector<std::pair<std::string, std::string>>(std::string_view prefix)> findModuleSymbols = {};
};

/**
 * @brief Computes completion items based on lexical scope and member access context.
 * @param request Immutable context for completion.
 * @return Vector of CompletionItem objects.
 */
std::vector<lsp::CompletionItem> GetCompletion(const CompletionRequest& request);

/**
 * @brief Context for filling in the details of one completion item.
 */
struct CompletionResolveRequest
{
    /** @brief The item the client picked, carrying the identity GetCompletion put on `data`. */
    const lsp::CompletionItem& item;

    const analysis::SymbolTable& symbolTable;

    /**
     * @brief Reads a document's text by URI, or returns nullptr when the server holds none.
     *
     * Injected rather than taken as a map: the documentation lives in the file that declares
     * the symbol, which is rarely the file completion was invoked in, and only the server
     * knows which documents it currently holds text for.
     */
    std::function<const std::string*(const std::string&)> readDocument = {};
};

/**
 * @brief Fills in the documentation of a completion item, on demand.
 *
 * Extracting a doc comment means finding the declaring file and re-scanning the lines above the
 * declaration. Doing that for every item of every completion request costs far more than doing
 * it once for the item the user actually highlighted, which is what this exists for.
 *
 * @param request Item to complete plus the lookups needed to do it.
 * @return The item with documentation attached, or unchanged when there is none to add.
 */
lsp::CompletionItem ResolveCompletionItem(const CompletionResolveRequest& request);

/**
 * @brief Relative proximity ranking of candidate symbols for typo correction suggestions.
 */
enum class SymbolProximity : uint8_t
{
    Local = 0,
    CurrentFile = 1,
    ExternalFile = 2,
    Predefined = 3,
    Snippet = 4,
    Keyword = 5
};

/**
 * @brief Candidate completion item with ranking score, documentation, and snippet text.
 */
struct CompletionCandidate
{
    std::string label;
    lsp::CompletionItemKind kind = lsp::CompletionItemKind::Variable;
    std::string detail = {};
    std::string doc = {};
    std::string resolveKey = {};
    std::string snippet = {};
    std::string sortText = {};
    std::string insertText = {};
};

/**
 * @brief Accumulates completion candidate items and applies ranking weights.
 */
struct CompletionCollector
{
    std::vector<lsp::CompletionItem>& items;
    std::unordered_set<std::string>& seenLabels;
    const CompletionRequest& request;
};

/**
 * @brief Completion snippet template and display description for an AngelScript operator.
 */
struct OperatorSnippet
{
    const char* label;
    const char* snippet;
    const char* detail;
};

/**
 * @brief Tracks enclosing scope kind, callable boundary, and access rules during completion resolution.
 */
enum class LexicalContext
{
    Code,
    Comment,
    StringLiteral,
};

/**
 * @brief Hierarchy traversal context for inheriting member completions from base types.
 */
struct InheritanceContext
{
    analysis::SymbolType declKind = analysis::SymbolType::Class;
    std::string declaringClassName;
    std::string qualifier;
};

/**
 * @brief Template parameter substitution context during member completion resolution.
 */
struct MemberTemplateContext
{
    const analysis::TemplateBinding& binding;
    const std::vector<std::string>& templateArgs;
    std::string_view memberPrefix = "";
};

/**
 * @brief Result of peeling array subscript or template container dimensions from an expression type.
 */
struct IndexPeelResult
{
    size_t count = 0;
    std::string lastIndexExpr;
};

/**
 * @brief Ranking penalty category for argument-to-parameter type conversion during overload resolution.
 */
enum class TypeMatchRank : uint8_t
{
    Exact = 0,
    Convertible = 1,
    Other = 2
};
} // namespace angel_lsp::features
