#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Categorical kind of a code action produced by the analysis engine.
 * @note The protocol layer maps each value explicitly.
 */
enum class CodeActionEntryKind : uint8_t
{
    QuickFix,             ///< Corrects problems identified by diagnostics.
    RefactorExtract,      ///< Extracts code into a reusable declaration.
    RefactorRewrite,      ///< Rewrites or restructures existing expressions or statements.
    SourceOrganizeImports ///< Reorders and cleans document include directives or imports.
};

/**
 * @brief Text replacement applied to a specific range in the target document.
 */
struct CodeActionTextEdit
{
    SourceRange range;   ///< Source range in the document to replace.
    std::string newText; ///< Replacement text inserted over the specified range.
};

/**
 * @brief Category of command executed when a client accepts a code action.
 */
enum class CodeActionCommandKind : uint8_t
{
    DisableDiagnostic, ///< Disables a diagnostic rule via configuration settings.
    IgnoreUnusedSymbol ///< Adds a symbol to the workspace unused symbol suppression list.
};

/**
 * @brief Command payload dispatched when executing an action; the protocol layer owns
 *        command identifiers and argument layout.
 */
struct CodeActionCommandEntry
{
    CodeActionCommandKind kind = CodeActionCommandKind::DisableDiagnostic; ///< Action command kind to execute.
    std::string title;      ///< Human-readable command title presented in the editor.
    std::string setting;    ///< Configuration setting key used by DisableDiagnostic.
    std::string code;       ///< Diagnostic code string used by DisableDiagnostic.
    std::string symbolName; ///< Symbol name identifier used by IgnoreUnusedSymbol.
};

/**
 * @brief Protocol-free code action entry; optional fields mirror protocol payload presence
 *        while text edits target the queried document.
 */
struct CodeActionEntry
{
    std::string title;                                        ///< Descriptive action title presented to the user.
    CodeActionEntryKind kind = CodeActionEntryKind::QuickFix; ///< Categorical kind of this code action.
    std::optional<std::vector<CodeActionTextEdit>> edits;     ///< Document edits targeting the queried document.
    std::optional<std::vector<uint32_t>> diagnosticIndices;   ///< Indices into CodeActionQuery::diagnostics.
    std::optional<bool> isPreferred;                          ///< Marks this action as the preferred resolution.
    std::optional<CodeActionCommandEntry> command;            ///< Optional client command executed with this action.
};

/**
 * @brief Lightweight view into an LSP diagnostic provided in the request context.
 * @note Views reference caller request diagnostics and code is empty for numeric or absent codes.
 */
struct CodeActionDiagnosticView
{
    SourceRange range;              ///< Source range where the diagnostic applies.
    std::string_view code;          ///< Diagnostic code, or empty for numeric or absent codes.
    std::string_view message;       ///< Diagnostic message text.
    bool messageIsPlainText = true; ///< Indicates whether the message text is plain text.
};

/**
 * @brief Encapsulates all document state, index context, and options for one query.
 * @warning String views and diagnostic spans must outlive the query execution.
 */
struct CodeActionQuery
{
    std::string_view uri;                                  ///< URI of the document being queried.
    std::string_view sourceCode;                           ///< Full source text of the target document.
    const NodeIndex* nodeIndex = nullptr;                  ///< Syntax revision node index; null if unparsed.
    SourceRange range;                                     ///< Selected or active editor source range.
    std::span<const CodeActionDiagnosticView> diagnostics; ///< Diagnostics overlapping the query range.
    const SymbolTable& symbolTable;                        ///< Workspace symbol table for lookups.
    const ScopeIndex& scopeIndex;                          ///< Lexical scope index for the document.
    std::span<const std::string> allowedRoots;             ///< Allowed include roots for organize-imports.
    const config::ServerConfig* config = nullptr;          ///< Active server configuration; null uses defaults.
    const i18n::I18n* i18n = nullptr;                      ///< Translation context for localized action titles.
};

} // namespace angel_lsp::analysis::code_action
