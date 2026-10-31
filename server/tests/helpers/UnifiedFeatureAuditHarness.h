#pragma once

#include "features/code_action/CodeActionHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "helpers/LspSemanticHarnessFixture.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::test
{

/**
 * @brief Telemetry audit thresholds for LSP language feature requests.
 */
struct FeatureTelemetryLimits
{
    double maxHoverMs = 1.0;
    double maxCompletionMs = 5.0;
    double maxDefinitionMs = 5.0;
    double maxReferencesMs = 5.0;
    double maxCodeActionsMs = 5.0;
    double maxSemanticTokensMs = 20.0;
};

/**
 * @brief Unified harness for multi-feature LSP audits, parity oracle tests,
 *        and latency budget assertions across good, bad, and boundary paths.
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
     * @brief Audits Hover latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return Hover response.
     */
    std::optional<lsp::Hover> AuditHover(const std::string& uri, uint32_t line, uint32_t col, double maxMs = 1.0);

    /**
     * @brief Audits Completion latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return Vector of completion items.
     */
    std::vector<lsp::CompletionItem> AuditCompletion(const std::string& uri, uint32_t line, uint32_t col,
                                                     double maxMs = 5.0);

    /**
     * @brief Audits Definition latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return Optional vector of locations.
     */
    std::optional<std::vector<lsp::Location>> AuditDefinition(const std::string& uri, uint32_t line, uint32_t col,
                                                              double maxMs = 5.0);

    /**
     * @brief Audits References latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] line 0-indexed line number.
     * @param[in] col 0-indexed column number.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return Optional vector of locations.
     */
    std::optional<std::vector<lsp::Location>> AuditReferences(const std::string& uri, uint32_t line, uint32_t col,
                                                              double maxMs = 5.0);

    /**
     * @brief Audits Code Actions latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] range Range in document.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return Optional vector of code actions.
     */
    std::optional<std::vector<lsp::CodeAction>> AuditCodeActions(const std::string& uri, const lsp::Range& range,
                                                                 double maxMs = 5.0);

    /**
     * @brief Audits Semantic Tokens latency and asserts it stays within the budget.
     * @param[in] uri Virtual document URI.
     * @param[in] maxMs Max allowed latency in milliseconds.
     * @return SemanticTokens struct.
     */
    lsp::SemanticTokens AuditSemanticTokens(const std::string& uri, double maxMs = 20.0);

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
