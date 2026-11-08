/**
 * @file CodeActionSuppressions.cpp
 * @brief Quick-fix code actions to suppress diagnostics via comments.
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Creates a code action entry offering a line-level diagnostic suppression comment.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Target diagnostic index to suppress.
 * @param[in] aliasOrCode Diagnostic code or human-readable alias.
 * @return Constructed line suppression CodeActionEntry.
 */
static CodeActionEntry MakeLineSuppressionEntry(const CodeActionQuery& query, uint32_t diagIndex,
                                                std::string_view aliasOrCode)
{
    const auto& diag = query.diagnostics[diagIndex];
    const uint32_t line = diag.range.startLine;
    const std::string_view lineStr = utils::GetLine(query.sourceCode, line);
    const uint32_t col = static_cast<uint32_t>(lineStr.size());

    CodeActionTextEdit edit{analysis::SourceRange{line, col, line, col},
                            " // disable-line " + std::string(aliasOrCode)};

    std::string title =
        i18n::FormatMessage(query.i18n, "action-suppress-line", "Disable {} for this line", aliasOrCode);

    return MakeQuickFixEntry(QuickFixEntryOptions{std::move(title), diagIndex, {std::move(edit)}, false});
}

/**
 * @brief Creates a code action entry offering a range-level diagnostic suppression block.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Target diagnostic index to suppress.
 * @param[in] aliasOrCode Diagnostic code or human-readable alias.
 * @return Constructed range suppression CodeActionEntry.
 */
static CodeActionEntry MakeRangeSuppressionEntry(const CodeActionQuery& query, uint32_t diagIndex,
                                                 std::string_view aliasOrCode)
{
    const auto& diag = query.diagnostics[diagIndex];
    const uint32_t startLine = diag.range.startLine;
    const uint32_t endLine = diag.range.endLine;
    const std::string startIndent = GetLineIndentation(query.sourceCode, startLine);
    const std::string endIndent = GetLineIndentation(query.sourceCode, endLine);
    const std::string_view endLineStr = utils::GetLine(query.sourceCode, endLine);
    const uint32_t endCol = static_cast<uint32_t>(endLineStr.size());

    CodeActionTextEdit startEdit{analysis::SourceRange{startLine, 0, startLine, 0},
                                 startIndent + "// disable " + std::string(aliasOrCode) + "\n"};

    CodeActionTextEdit endEdit{analysis::SourceRange{endLine, endCol, endLine, endCol},
                               "\n" + endIndent + "// enable " + std::string(aliasOrCode)};

    std::string title = i18n::FormatMessage(query.i18n, "action-suppress-range",
                                            "Disable {} with // disable ... // enable", aliasOrCode);

    return MakeQuickFixEntry(
        QuickFixEntryOptions{std::move(title), diagIndex, {std::move(startEdit), std::move(endEdit)}, false});
}

/**
 * @brief Creates a code action entry offering a file-level diagnostic suppression header comment.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Target diagnostic index to suppress.
 * @param[in] aliasOrCode Diagnostic code or human-readable alias.
 * @return Constructed file suppression CodeActionEntry.
 */
static CodeActionEntry MakeFileSuppressionEntry(const CodeActionQuery& query, uint32_t diagIndex,
                                                std::string_view aliasOrCode)
{

    CodeActionTextEdit edit{analysis::SourceRange{0, 0, 0, 0}, "// disable " + std::string(aliasOrCode) + "\n"};

    std::string title =
        i18n::FormatMessage(query.i18n, "action-suppress-file", "Disable {} for entire file", aliasOrCode);

    return MakeQuickFixEntry(QuickFixEntryOptions{std::move(title), diagIndex, {std::move(edit)}, false});
}

/**
 * @brief Generates quick-fix entries to suppress diagnostics via comments.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddDiagnosticSuppressionFixes(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (query.sourceCode.empty() || query.diagnostics.empty())
    {
        return;
    }

    std::unordered_set<std::string_view> seenCodes;
    std::unordered_set<std::string_view> seenGroups;
    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto rawCode = query.diagnostics[i].code;
        if (rawCode.empty() || !seenCodes.insert(rawCode).second)
        {
            continue;
        }

        const std::string_view alias = analysis::GetDiagnosticAlias(rawCode);
        const std::string_view displayCode = alias.empty() ? rawCode : alias;

        entries.push_back(MakeLineSuppressionEntry(query, i, displayCode));
        entries.push_back(MakeRangeSuppressionEntry(query, i, displayCode));
        entries.push_back(MakeFileSuppressionEntry(query, i, displayCode));
        const auto group = analysis::GetDiagnosticGroup(rawCode);
        if (!group.empty() && seenGroups.insert(group).second)
        {
            entries.push_back(MakeFileSuppressionEntry(query, i, group));
        }
    }
}

} // namespace angel_lsp::analysis::code_action
