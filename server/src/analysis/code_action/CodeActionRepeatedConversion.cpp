/**
 * @file CodeActionRepeatedConversion.cpp
 * @brief Quick-fix code action to cache repeated conversions in if-else ladders.
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/index/RepeatedConversionFacts.h"
#include "parser/Keywords.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Extracts expression and type names from a repeated-conversion diagnostic message.
 * @param[in] diag Diagnostic view to inspect.
 * @return Extracted diagnostic information or std::nullopt if unmatched or malformed.
 */
static std::optional<ExtractedDiagInfo> ParseDiagnosticMessage(const CodeActionDiagnosticView& diag)
{
    if (!MatchDiagnosticCode(diag, "as-hint-repeated-conversion") || !diag.messageIsPlainText)
    {
        return std::nullopt;
    }
    const std::string_view msg = diag.message;
    const size_t q1 = msg.find('\'');
    const size_t q2 = (q1 == std::string_view::npos) ? std::string_view::npos : msg.find('\'', q1 + 1);
    const size_t q3 = (q2 == std::string_view::npos) ? std::string_view::npos : msg.find('\'', q2 + 1);
    const size_t q4 = (q3 == std::string_view::npos) ? std::string_view::npos : msg.find('\'', q3 + 1);
    const size_t q5 = (q4 == std::string_view::npos) ? std::string_view::npos : msg.find('\'', q4 + 1);
    const size_t q6 = (q5 == std::string_view::npos) ? std::string_view::npos : msg.find('\'', q5 + 1);
    if (q6 == std::string_view::npos)
    {
        return std::nullopt;
    }

    return ExtractedDiagInfo{std::string(msg.substr(q1 + 1, q2 - q1 - 1)), std::string(msg.substr(q3 + 1, q4 - q3 - 1)),
                             std::string(msg.substr(q5 + 1, q6 - q5 - 1))};
}

/**
 * @brief Derives a suitable local variable name from an expression and target type.
 * @param[in] exprText Source expression text.
 * @param[in] toType Target conversion type name.
 * @return Derived variable name identifier.
 */
static std::string DeriveVariableName(std::string_view exprText, std::string_view toType)
{
    const size_t lastDot = exprText.rfind('.');
    std::string candidate = (lastDot != std::string_view::npos && lastDot + 1 < exprText.size())
                                ? std::string(exprText.substr(lastDot + 1))
                                : std::string(exprText);
    if (candidate.starts_with("m_") && candidate.size() > 2)
    {
        candidate = candidate.substr(2);
    }
    if (!candidate.empty())
    {
        candidate[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[0])));
    }
    if (candidate.empty() || parser::keywords::IsKeyword(candidate))
    {
        return !toType.empty() ? std::string(toType) : "cachedVar";
    }
    return candidate;
}

/**
 * @brief Constructs and appends a quick-fix entry caching the repeated conversion.
 * @param[in] sites Conversion occurrences and root statement position.
 * @param[in] info Extracted diagnostic parameters.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
static void BuildConversionEntry(const analysis::IfLadderConversionSites& sites, const ExtractedDiagInfo& info,
                                 const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    const std::string varName = DeriveVariableName(info.exprText, info.toType);
    const uint32_t stmtRow = sites.rootIfLine;
    const std::string indent = GetLineIndentation(query.sourceCode, stmtRow);

    std::vector<CodeActionTextEdit> textEdits;
    textEdits.push_back(CodeActionTextEdit{analysis::SourceRange{stmtRow, 0, stmtRow, 0},
                                           indent + info.toType + " " + varName + " = " + info.exprText + ";\n"});
    for (const auto& occ : sites.occurrences)
    {
        textEdits.push_back(CodeActionTextEdit{occ, varName});
    }

    CodeActionEntry entry;
    entry.title =
        i18n::FormatMessage(query.i18n, "action-init-local-variable", "Initialize local '{}' variable for '{}'",
                            std::string_view(info.toType), std::string_view(info.exprText));
    entry.kind = CodeActionEntryKind::QuickFix;
    entry.isPreferred = true;
    entry.edits = std::move(textEdits);

    entries.push_back(std::move(entry));
}

/**
 * @brief Generates quick-fix entries to cache repeated conversions in an if-ladder.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddRepeatedConversionFix(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : query.diagnostics)
    {
        const auto info = ParseDiagnosticMessage(diag);
        if (!info)
        {
            continue;
        }

        const auto sites =
            analysis::FindIfLadderConversionSites(*query.nodeIndex, query.sourceCode, diag.range, info->exprText);
        if (!sites)
        {
            continue;
        }

        BuildConversionEntry(*sites, *info, query, entries);
        break;
    }
}

} // namespace angel_lsp::analysis::code_action
