#include "features/code_action/CodeActionInternal.h"
#include "analysis/ASTUtils.h"
#include "analysis/RepeatedConversionChecker.h"
#include "parser/GrammarNames.h"
#include "parser/Keywords.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
namespace
{
struct ExtractedDiagInfo
{
    std::string exprText;
    std::string fromType;
    std::string toType;
};

std::optional<ExtractedDiagInfo> ParseDiagnosticMessage(const lsp::Diagnostic& diag)
{
    if (!MatchDiagnosticCode(diag, "as-hint-repeated-conversion") || !std::holds_alternative<lsp::String>(diag.message))
    {
        return std::nullopt;
    }
    const std::string& msg = std::get<lsp::String>(diag.message);
    const size_t q1 = msg.find('\'');
    const size_t q2 = (q1 == std::string::npos) ? std::string::npos : msg.find('\'', q1 + 1);
    const size_t q3 = (q2 == std::string::npos) ? std::string::npos : msg.find('\'', q2 + 1);
    const size_t q4 = (q3 == std::string::npos) ? std::string::npos : msg.find('\'', q3 + 1);
    const size_t q5 = (q4 == std::string::npos) ? std::string::npos : msg.find('\'', q4 + 1);
    const size_t q6 = (q5 == std::string::npos) ? std::string::npos : msg.find('\'', q5 + 1);
    if (q6 == std::string::npos)
    {
        return std::nullopt;
    }

    return ExtractedDiagInfo{msg.substr(q1 + 1, q2 - q1 - 1), msg.substr(q3 + 1, q4 - q3 - 1),
                             msg.substr(q5 + 1, q6 - q5 - 1)};
}

std::string DeriveVariableName(std::string_view exprText, std::string_view toType)
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

TSNode FindRootIfStatement(TSNode node)
{
    TSNode curr = node;
    TSNode rootIf = {};
    while (!ts_node_is_null(curr))
    {
        if (std::string_view(ts_node_type(curr)) == "if_statement")
        {
            rootIf = curr;
        }
        curr = ts_node_parent(curr);
    }
    return rootIf;
}

void BuildCodeActionEdits(TSNode rootIf, const ExtractedDiagInfo& info, const CodeActionRequest& request,
                          std::vector<lsp::CodeAction>& actions)
{
    const std::string varName = DeriveVariableName(info.exprText, info.toType);
    const uint32_t stmtRow = ts_node_start_point(rootIf).row;
    const std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

    lsp::TextEdit declEdit;
    declEdit.range = lsp::Range{{stmtRow, 0}, {stmtRow, 0}};
    declEdit.newText = indent + info.toType + " " + varName + " = " + info.exprText + ";\n";

    const auto occNodes = analysis::FindOccurrencesInIfLadder(rootIf, info.exprText, request.sourceCode);

    std::vector<lsp::TextEdit> edits;
    edits.push_back(std::move(declEdit));
    for (TSNode occ : occNodes)
    {
        const TSPoint s = ts_node_start_point(occ);
        const TSPoint e = ts_node_end_point(occ);
        lsp::TextEdit repl;
        repl.range = lsp::Range{{s.row, s.column}, {e.row, e.column}};
        repl.newText = varName;
        edits.push_back(std::move(repl));
    }

    lsp::CodeAction action;
    action.title = "Initialize local '" + info.toType + "' variable for '" + info.exprText + "'";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = std::move(edits);
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}
} // namespace

void TryAddRepeatedConversionFix(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        const auto info = ParseDiagnosticMessage(diag);
        if (!info)
        {
            continue;
        }

        const TSPoint startPt = {diag.range.start.line, diag.range.start.character};
        const TSPoint endPt = {diag.range.end.line, diag.range.end.character};
        TSNode targetNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
        TSNode rootIf = FindRootIfStatement(targetNode);
        if (ts_node_is_null(rootIf))
        {
            continue;
        }

        BuildCodeActionEdits(rootIf, *info, request, actions);
        break;
    }
}
} // namespace angel_lsp::features
