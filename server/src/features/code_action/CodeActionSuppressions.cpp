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
    uint32_t line = diag.range.start.line;
    std::string_view lineStr = utils::GetLine(request.sourceCode, line);

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{line, static_cast<uint32_t>(lineStr.size())};
    edit.range.end = lsp::Position{line, static_cast<uint32_t>(lineStr.size())};
    edit.newText = " // disable-line " + std::string(aliasOrCode);

    lsp::CodeAction action;
    action.title = "Disable " + std::string(aliasOrCode) + " for this line";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

void AddRangeSuppressionFix(const CodeActionRequest& request, const lsp::Diagnostic& diag, std::string_view aliasOrCode,
                            std::vector<lsp::CodeAction>& actions)
{
    uint32_t startLine = diag.range.start.line;
    uint32_t endLine = diag.range.end.line;
    std::string startIndent = GetLineIndentation(request.sourceCode, startLine);
    std::string endIndent = GetLineIndentation(request.sourceCode, endLine);
    std::string_view endLineStr = utils::GetLine(request.sourceCode, endLine);

    lsp::TextEdit startEdit;
    startEdit.range.start = lsp::Position{startLine, 0};
    startEdit.range.end = lsp::Position{startLine, 0};
    startEdit.newText = startIndent + "// disable " + std::string(aliasOrCode) + "\n";

    lsp::TextEdit endEdit;
    endEdit.range.start = lsp::Position{endLine, static_cast<uint32_t>(endLineStr.size())};
    endEdit.range.end = lsp::Position{endLine, static_cast<uint32_t>(endLineStr.size())};
    endEdit.newText = "\n" + endIndent + "// enable " + std::string(aliasOrCode);

    lsp::CodeAction action;
    action.title = "Disable " + std::string(aliasOrCode) + " with // disable ... // enable";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(startEdit), std::move(endEdit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

void AddFileSuppressionFix(const CodeActionRequest& request, const lsp::Diagnostic& diag, std::string_view aliasOrCode,
                           std::vector<lsp::CodeAction>& actions)
{
    lsp::TextEdit edit;
    edit.range.start = lsp::Position{0, 0};
    edit.range.end = lsp::Position{0, 0};
    edit.newText = "// disable " + std::string(aliasOrCode) + "\n";

    lsp::CodeAction action;
    action.title = "Disable " + std::string(aliasOrCode) + " for entire file";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
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
