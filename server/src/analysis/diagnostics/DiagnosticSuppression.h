#pragma once

#include "analysis/diagnostics/Diagnostics.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Returns the numeric alias (e.g. "W156", "E101") for a canonical diagnostic code.
 * @param[in] canonicalCode Canonical string code (e.g. "as-hint-list-pattern-unknown").
 * @return Numeric alias if defined, or empty view.
 */
[[nodiscard]] std::string_view GetDiagnosticAlias(std::string_view canonicalCode) noexcept;

/**
 * @brief Returns the canonical diagnostic code for a numeric alias or string code.
 * @param[in] aliasOrCode Numeric alias (e.g. "W156", "w156") or canonical code.
 * @return Canonical code if recognized, or empty view.
 */
[[nodiscard]] std::string_view GetCanonicalDiagnosticCode(std::string_view aliasOrCode) noexcept;

/**
 * @brief Normalizes a code, group, or historical alias to its stable selector.
 * @param[in] selector Code or group as written in a comment or configuration.
 * @return Normalized selector or the original spelling for an unrecognized value.
 */
[[nodiscard]] std::string_view NormalizeDiagnosticSelector(std::string_view selector) noexcept;

/**
 * @brief Returns the thematic group for a diagnostic.
 * @param[in] code Canonical code or numeric alias.
 * @return Group name, or an empty view for diagnostics without a thematic group.
 */
[[nodiscard]] std::string_view GetDiagnosticGroup(std::string_view code) noexcept;

/**
 * @brief Returns the broad diagnostic category determined by the canonical code.
 * @param[in] code Canonical code or numeric alias.
 * @return Category name for warnings and hints, or an empty view otherwise.
 */
[[nodiscard]] std::string_view GetDiagnosticCategory(std::string_view code) noexcept;

/**
 * @brief Formats a display code for LSP diagnostics (e.g. "W156" or "as-err-...").
 * @param[in] canonicalCode Canonical string code.
 * @return The static numeric alias when one exists, otherwise a view of canonicalCode itself.
 */
[[nodiscard]] std::string_view FormatDiagnosticDisplayCode(std::string_view canonicalCode) noexcept;

/**
 * @brief Active suppression range for a diagnostic code.
 */
struct DiagnosticSuppression
{
    std::string_view code; ///< Canonical diagnostic code, group, category or "all"; views a static table or the source.
    uint32_t startLine = 0;
    uint32_t endLine = UINT32_MAX;
};

/**
 * @brief Map of suppressions per file with fast query support.
 * @warning Selectors that are not known codes view the parsed source text; the map must not outlive it.
 */
class DiagnosticSuppressionMap
{
  public:
    /**
     * @brief Records a suppressed line range for a selector after normalizing aliases and group names.
     * @param[in] code Diagnostic code, alias, group or category selector; must outlive the map.
     * @param[in] startLine First suppressed zero-based line.
     * @param[in] endLine Last suppressed zero-based line, inclusive.
     */
    void AddSuppression(std::string_view code, uint32_t startLine, uint32_t endLine);

    /**
     * @brief Checks whether a diagnostic code is suppressed on a line by its code, group, category or "all".
     * @param[in] code Diagnostic code or alias.
     * @param[in] line Zero-based line of the diagnostic.
     * @return True when any recorded range covering the line matches the code.
     */
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
 * @param[in] sourceCode Script source text; must outlive the returned map.
 * @param[in] nodeIndex Optional first-pass index whose comment ranges replace the line-by-line text scan.
 * @return DiagnosticSuppressionMap populated with active line ranges.
 */
DiagnosticSuppressionMap ParseDiagnosticSuppressions(std::string_view sourceCode, const NodeIndex* nodeIndex = nullptr);

/**
 * @brief Associates diagnostic code string identifiers with human-readable alias representations.
 */
struct CodeAliasEntry
{
    std::string_view alias;
    std::string_view code;
};
} // namespace angel_lsp::analysis
