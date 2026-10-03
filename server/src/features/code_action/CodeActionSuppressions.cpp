/**
 * @file CodeActionSuppressions.cpp
 * @brief Quick-fix code actions to suppress diagnostics via comments.
 */

#include "analysis/DiagnosticSuppression.h"
#include "features/code_action/CodeActionInternal.h"
#include <unordered_set>

namespace angel_lsp::features
{
namespace
{

void AddLineSuppressionFix(const CodeActionRequest& request, const lsp::Diagnostic& diag, std::string_view aliasOrCode,
                           std::vector<lsp::CodeAction>& actions)
{
    const uint32_t line = diag.range.start.line;
    const std::string_view lineStr = utils::GetLine(request.sourceCode, line);

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{line, static_cast<uint32_t>(lineStr.size())};
    edit.range.end = lsp::Position{line, static_cast<uint32_t>(lineStr.size())};
    edit.newText = " // disable-line " + std::string(aliasOrCode);

    actions.push_back(MakeQuickFixAction(
        i18n::FormatMessage(request.i18n, "action-suppress-line", "Disable {} for this line", aliasOrCode), diag,
        request.uri, {std::move(edit)}));
}

void AddRangeSuppressionFix(const CodeActionRequest& request, const lsp::Diagnostic& diag, std::string_view aliasOrCode,
                            std::vector<lsp::CodeAction>& actions)
{
    const uint32_t startLine = diag.range.start.line;
    const uint32_t endLine = diag.range.end.line;
    const std::string startIndent = GetLineIndentation(request.sourceCode, startLine);
    const std::string endIndent = GetLineIndentation(request.sourceCode, endLine);
    const std::string_view endLineStr = utils::GetLine(request.sourceCode, endLine);

    lsp::TextEdit startEdit;
    startEdit.range.start = lsp::Position{startLine, 0};
    startEdit.range.end = lsp::Position{startLine, 0};
    startEdit.newText = startIndent + "// disable " + std::string(aliasOrCode) + "\n";

    lsp::TextEdit endEdit;
    endEdit.range.start = lsp::Position{endLine, static_cast<uint32_t>(endLineStr.size())};
    endEdit.range.end = lsp::Position{endLine, static_cast<uint32_t>(endLineStr.size())};
    endEdit.newText = "\n" + endIndent + "// enable " + std::string(aliasOrCode);

    actions.push_back(MakeQuickFixAction(i18n::FormatMessage(request.i18n, "action-suppress-range",
                                                             "Disable {} with // disable ... // enable", aliasOrCode),
                                         diag, request.uri, {std::move(startEdit), std::move(endEdit)}));
}

void AddFileSuppressionFix(const CodeActionRequest& request, const lsp::Diagnostic& diag, std::string_view aliasOrCode,
                           std::vector<lsp::CodeAction>& actions)
{
    lsp::TextEdit edit;
    edit.range.start = lsp::Position{0, 0};
    edit.range.end = lsp::Position{0, 0};
    edit.newText = "// disable " + std::string(aliasOrCode) + "\n";

    actions.push_back(MakeQuickFixAction(
        i18n::FormatMessage(request.i18n, "action-suppress-file", "Disable {} for entire file", aliasOrCode), diag,
        request.uri, {std::move(edit)}));
}

} // namespace

void TryAddDiagnosticSuppressionFixes(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (request.sourceCode.empty() || request.context.diagnostics.empty())
    {
        return;
    }

    std::unordered_set<std::string> seenCodes;
    for (const auto& diag : request.context.diagnostics)
    {
        if (!diag.code.has_value() || !std::holds_alternative<lsp::String>(diag.code.value()))
        {
            continue;
        }

        const auto& rawCode = std::get<lsp::String>(diag.code.value());
        if (rawCode.empty() || seenCodes.contains(rawCode))
        {
            continue;
        }
        seenCodes.insert(rawCode);

        std::string_view alias = analysis::GetDiagnosticAlias(rawCode);
        std::string_view displayCode = alias.empty() ? std::string_view(rawCode) : alias;

        AddLineSuppressionFix(request, diag, displayCode, actions);
        AddRangeSuppressionFix(request, diag, displayCode, actions);
        AddFileSuppressionFix(request, diag, displayCode, actions);
    }
}

} // namespace angel_lsp::features
