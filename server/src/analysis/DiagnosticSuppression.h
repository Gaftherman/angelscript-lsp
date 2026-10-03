#pragma once

#include "analysis/Diagnostics.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
class NodeIndex;

/**
 * @brief Returns the numeric alias (e.g. "W156", "E101") for a canonical diagnostic code.
 * @param canonicalCode Canonical string code (e.g. "as-hint-list-pattern-unknown").
 * @return Numeric alias if defined, or empty view.
 */
[[nodiscard]] std::string_view GetDiagnosticAlias(std::string_view canonicalCode) noexcept;

/**
 * @brief Returns the canonical diagnostic code for a numeric alias or string code.
 * @param aliasOrCode Numeric alias (e.g. "W156", "w156") or canonical code.
 * @return Canonical code if recognized, or empty view.
 */
[[nodiscard]] std::string_view GetCanonicalDiagnosticCode(std::string_view aliasOrCode) noexcept;

/**
 * @brief Formats a display code for LSP diagnostics (e.g. "W156" or "as-err-...").
 * @param canonicalCode Canonical string code.
 * @return Display string preferring the numeric alias when available.
 */
[[nodiscard]] std::string FormatDiagnosticDisplayCode(std::string_view canonicalCode);

/**
 * @brief Represents an active suppression range for a diagnostic code.
 */
struct DiagnosticSuppression
{
    std::string code; ///< Canonical diagnostic code, or "all".
    uint32_t startLine = 0;
    uint32_t endLine = UINT32_MAX;
};

/**
 * @brief Map of suppressions per file with fast query support.
 */
class DiagnosticSuppressionMap
{
  public:
    void AddSuppression(std::string_view code, uint32_t startLine, uint32_t endLine);
    [[nodiscard]] bool IsSuppressed(std::string_view code, uint32_t line) const;
    [[nodiscard]] bool Empty() const noexcept
    {
        return m_suppressions.empty();
    }

  private:
    std::vector<DiagnosticSuppression> m_suppressions;
};

/**
 * @brief Parses diagnostic suppressions (// disable <CODE>, // enable <CODE>, etc.) from source code.
 * @param sourceCode Script source text.
 * @param nodeIndex Optional pre-indexed Tree-Sitter AST node index for fast comment lookup.
 * @return DiagnosticSuppressionMap populated with active line ranges.
 */
DiagnosticSuppressionMap ParseDiagnosticSuppressions(std::string_view sourceCode, const NodeIndex* nodeIndex = nullptr);

} // namespace angel_lsp::analysis
