#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "config/ServerConfig.h"
#include "utils/TransparentHash.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Kind of a completion entry, limited to the kinds the engine produces.
 * @note The protocol layer maps each value explicitly; the numbers carry no protocol meaning.
 */
enum class CompletionEntryKind : uint8_t
{
    Method = 2,
    Function = 3,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Enum = 13,
    Keyword = 14,
    Snippet = 15,
    File = 17,
    EnumMember = 20,
    TypeParameter = 25
};

/**
 * @brief Single-line replacement applied when an include-path entry is accepted.
 */
struct CompletionTextEdit
{
    uint32_t line = 0;           ///< 0-based line of the replaced text.
    uint32_t startCharacter = 0; ///< First replaced column: the one after the opening quote.
    uint32_t endCharacter = 0;   ///< One past the last replaced column: the cursor column.
    std::string newText;         ///< Text written over the range.
};

/**
 * @brief Protocol-free completion entry; the feature layer maps each one to an LSP completion item.
 *        Optional fields stay empty when the engine has nothing to say, so the client applies its defaults.
 */
struct CompletionEntry
{
    std::string label;                          ///< Text shown in the list and inserted by default.
    std::optional<CompletionEntryKind> kind;    ///< Icon category.
    std::optional<std::string> detail;          ///< Type or signature shown beside the label.
    std::optional<std::string> insertText;      ///< Text inserted instead of the label.
    bool isSnippet = false;                     ///< True when `insertText` holds `${n:...}` placeholders.
    std::optional<std::string> sortText;        ///< Key the client sorts by instead of the label.
    std::optional<std::string> documentation;   ///< Markdown documentation shown with the entry.
    std::optional<std::string> resolveKey;      ///< Qualified symbol name for lazy documentation lookup.
    std::optional<CompletionTextEdit> textEdit; ///< Replacement used instead of plain insertion.
};

/**
 * @brief Everything the completion engine reads for one request.
 * @note The views point into the open document and the host's buffers; they must outlive the call.
 */
struct CompletionQuery
{
    std::string_view uri;                         ///< URI of the document being completed.
    std::string_view sourceCode;                  ///< Full text of that document.
    const NodeIndex* nodeIndex = nullptr;         ///< First-pass index of the same text; null when not parsed.
    const SymbolTable& symbolTable;               ///< Workspace symbols.
    const ScopeIndex& scopeIndex;                 ///< Lexical scopes per document.
    TargetPosition position;                      ///< Cursor line and column.
    const config::ServerConfig* config = nullptr; ///< Server settings; null uses the built-in defaults.

    /**
     * @brief Whether the client renders `${1:T}` placeholders; when false, plain text is inserted instead.
     */
    bool snippetSupport = false;

    /**
     * @brief Absolute path of the document, because include paths are offered relative to its directory.
     */
    std::string_view documentPath = {};

    /**
     * @brief Lists every file that may be named in an `#include`, as absolute paths.
     *        A callback because computing it walks the include graph, which only include completion needs.
     */
    std::function<std::vector<std::string>()> listIncludeCandidates = {};

    /**
     * @brief Extension left off an inserted include path when the host appends it itself; empty keeps it.
     */
    std::string_view implicitExtension = {};

    /**
     * @brief Returns module symbols starting with a prefix as (name, container) pairs; empty when unset.
     */
    std::function<std::vector<std::pair<std::string, std::string>>(std::string_view prefix)> findModuleSymbols = {};

    /**
     * @brief URIs of the files that make up this document's module (itself plus its `#include` closure).
     *        Global symbols declared in any other workspace file are not visible to the compiler, so they
     *        are not offered; predefined files are always visible. Null or empty offers every file.
     */
    const TransparentStringSet* moduleFileUris = nullptr;
};

/**
 * @brief Proximity tier of a candidate; lower tiers sort first.
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
 * @brief Raw attributes of one candidate before it is deduplicated and turned into an entry.
 *        Empty strings mean "not set": a snippet is used only when `insertText` is empty, and an empty
 *        `sortText` falls back to the external-file tier.
 */
struct CompletionCandidate
{
    std::string label;
    CompletionEntryKind kind = CompletionEntryKind::Variable;
    std::string detail = {};
    std::string doc = {};
    std::string resolveKey = {};
    std::string snippet = {};
    std::string sortText = {};
    std::string insertText = {};
};

/**
 * @brief Output list, the labels already offered, and the query, shared by every collection step.
 */
struct CompletionCollector
{
    std::vector<CompletionEntry>& items;
    std::unordered_set<std::string>& seenLabels;
    const CompletionQuery& request;
};

/**
 * @brief Snippet offered for an operator method in a class body.
 */
struct OperatorSnippet
{
    const char* label;
    const char* snippet;
    const char* detail;
};

/**
 * @brief Lexical region the cursor sits in; completion is offered only in code.
 */
enum class LexicalContext
{
    Code,
    Comment,
    StringLiteral,
};

/**
 * @brief What is being declared when the cursor is after `:` in a class or interface header.
 */
struct InheritanceContext
{
    SymbolType declKind = SymbolType::Class;
    std::string declaringClassName;
    std::string qualifier;
};

/**
 * @brief Template binding applied to member types when completing on an instantiated template.
 */
struct MemberTemplateContext
{
    const TemplateBinding& binding;
    const std::vector<std::string>& templateArgs;
    std::string_view memberPrefix = "";
};

/**
 * @brief Number of `[...]` subscripts peeled from a receiver and the text inside the last one.
 */
struct IndexPeelResult
{
    size_t count = 0;
    std::string lastIndexExpr;
};

/**
 * @brief How well an entry's type matches the type expected at the cursor; lower sorts first.
 */
enum class TypeMatchRank : uint8_t
{
    Exact = 0,
    Convertible = 1,
    Other = 2
};

} // namespace angel_lsp::analysis::completion
