#pragma once

#include "features/code_action/CodeActionHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/LspSemanticHarnessFixture.h"

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::test
{

/**
 * @brief Runs LSP feature audits and parity oracle tests with latency telemetry.
 */
class UnifiedFeatureAuditHarness : public LspSemanticHarnessFixture
{
  public:
    UnifiedFeatureAuditHarness() = default;
    ~UnifiedFeatureAuditHarness() = default;

    /**
     * @brief Requests definition for symbol under cursor.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Optional vector of resolved definition locations.
     */
    std::optional<std::vector<lsp::Location>> RequestDefinition(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Requests references for symbol under cursor.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Optional vector of resolved references.
     */
    std::optional<std::vector<lsp::Location>> RequestReferences(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Requests code actions for a range in a document.
     * @param[in] uri Virtual document URI.
     * @param[in] range LSP range.
     * @return Optional vector of available code actions.
     */
    std::optional<std::vector<lsp::CodeAction>> RequestCodeActions(const std::string& uri, const lsp::Range& range);

    /**
     * @brief Requests full semantic tokens for a document.
     * @param[in] uri Virtual document URI.
     * @return SemanticTokens struct containing encoded tokens.
     */
    lsp::SemanticTokens RequestSemanticTokens(const std::string& uri);

    /**
     * @brief Reports the latency of a hover request.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Hover response.
     */
    std::optional<lsp::Hover> AuditHover(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Reports the latency of a completion request.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Vector of completion items.
     */
    std::vector<lsp::CompletionItem> AuditCompletion(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Reports the latency of a definition request.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Optional vector of locations.
     */
    std::optional<std::vector<lsp::Location>> AuditDefinition(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Reports the latency of a references request.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @return Optional vector of locations.
     */
    std::optional<std::vector<lsp::Location>> AuditReferences(const std::string& uri, uint32_t line, uint32_t col);

    /**
     * @brief Reports the latency of a code action request.
     * @param[in] uri Virtual document URI.
     * @param[in] range Range in document.
     * @return Optional vector of code actions.
     */
    std::optional<std::vector<lsp::CodeAction>> AuditCodeActions(const std::string& uri, const lsp::Range& range);

    /**
     * @brief Reports the latency of a semantic tokens request.
     * @param[in] uri Virtual document URI.
     * @return SemanticTokens struct.
     */
    lsp::SemanticTokens AuditSemanticTokens(const std::string& uri);

    /**
     * @brief Executes 3-way verification: Good Path (0 diagnostics).
     * @param[in] relativePath Relative file path for sandbox.
     * @param[in] content AngelScript source content.
     * @return Document URI string.
     */
    std::string VerifyGoodPath(const std::string& relativePath, const std::string& content);

    /**
     * @brief Executes 3-way verification: Bad Path (exact expected diagnostic code).
     * @param[in] relativePath Relative file path for sandbox.
     * @param[in] content AngelScript source content.
     * @param[in] expectedCode Expected diagnostic error code.
     * @return Document URI string.
     */
    std::string VerifyBadPath(const std::string& relativePath, const std::string& content,
                              const std::string& expectedCode);
};

} // namespace angel_lsp::test
