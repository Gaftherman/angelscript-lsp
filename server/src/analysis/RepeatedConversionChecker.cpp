#include "analysis/RepeatedConversionChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/ComparisonOperatorChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "parser/QueryRegistry.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
struct ConversionOccurrence
{
    TSNode node;
    std::string exprText;
    std::string fromType;
    std::string toType;
};

bool IsExtractableCandidate(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const std::string_view type = ts_node_type(node);
    return type == "member_expression" || type == "identifier" || type == "call_expression" ||
           type == "index_expression" || type == "scoped_identifier";
}

void TryRecordOccurrence(TSNode operand, const std::string& fromType, const std::string& toType,
                         const DiagnosticContext& ctx, std::vector<ConversionOccurrence>& out)
{
    if (!IsExtractableCandidate(operand))
    {
        return;
    }
    if (TypeHasOpImplConvTo(fromType, toType, ctx.request.symbolTable))
    {
        out.push_back({operand, GetNodeText(operand, ctx.request.sourceCode), fromType, toType});
    }
}

void InspectComparisonNode(TSNode binNode, const Scope* scope, DiagnosticContext& ctx,
                           std::vector<ConversionOccurrence>& out)
{
    TSNode opNode = parser::GetChildByField(binNode, parser::fields::Operator);
    if (ts_node_is_null(opNode))
    {
        return;
    }
    const std::string op = GetNodeText(opNode, ctx.request.sourceCode);
    if (op != "==" && op != "!=")
    {
        return;
    }

    TSNode left = parser::GetChildByField(binNode, parser::fields::Left);
    TSNode right = parser::GetChildByField(binNode, parser::fields::Right);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const ExpressionTypeContext exprCtx{scope, ctx.request.symbolTable, ctx.request.sourceCode, ctx.request.fileUri};
    const std::string cleanLeft = CleanBaseType(ResolveExpressionType(left, exprCtx));
    const std::string cleanRight = CleanBaseType(ResolveExpressionType(right, exprCtx));
    if (cleanLeft.empty() || cleanRight.empty() || cleanLeft == cleanRight)
    {
        return;
    }

    TryRecordOccurrence(left, cleanLeft, cleanRight, ctx, out);
    TryRecordOccurrence(right, cleanRight, cleanLeft, ctx, out);
}

void CollectConditionConversions(TSNode condNode, const Scope* scope, DiagnosticContext& ctx,
                                 std::vector<ConversionOccurrence>& out)
{
    if (ts_node_is_null(condNode))
    {
        return;
    }

    const TSQuery* query = parser::QueryRegistry::GetBinaryExpressionQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (!query || !cursor)
    {
        return;
    }

    ts_query_cursor_exec(cursor, query, condNode);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint16_t i = 0; i < match.capture_count; ++i)
        {
            InspectComparisonNode(match.captures[i].node, scope, ctx, out);
        }
    }
}

TSNode GetIfCondition(TSNode ifNode)
{
    TSNode cond = parser::GetChildByField(ifNode, parser::fields::Condition);
    if (!ts_node_is_null(cond))
    {
        return cond;
    }
    return (ts_node_named_child_count(ifNode) > 0) ? ts_node_named_child(ifNode, 0) : TSNode{};
}

TSNode GetIfAlternative(TSNode ifNode)
{
    TSNode alt = parser::GetChildByField(ifNode, parser::fields::Alternative);
    if (!ts_node_is_null(alt))
    {
        return alt;
    }
    return (ts_node_named_child_count(ifNode) >= 3) ? ts_node_named_child(ifNode, 2) : TSNode{};
}

std::vector<TSNode> CollectLadderIfNodes(TSNode rootIfNode)
{
    std::vector<TSNode> ifNodes;
    TSNode curr = rootIfNode;
    while (!ts_node_is_null(curr) && std::string_view(ts_node_type(curr)) == "if_statement")
    {
        ifNodes.push_back(curr);
        curr = GetIfAlternative(curr);
    }
    return ifNodes;
}

void EmitRepeatedHints(const std::vector<ConversionOccurrence>& occurrences, DiagnosticContext& ctx)
{
    if (occurrences.size() < 2)
    {
        return;
    }

    std::unordered_map<std::string, size_t> counts;
    for (const auto& occ : occurrences)
    {
        ++counts[occ.exprText];
    }

    for (const auto& occ : occurrences)
    {
        if (counts[occ.exprText] >= 2)
        {
            const TSPoint start = ts_node_start_point(occ.node);
            const TSPoint end = ts_node_end_point(occ.node);
            ctx.EmitAtRange({start.row, start.column, end.row, end.column}, diagnostics::codes::RepeatedConversion,
                            {occ.exprText, occ.fromType, occ.toType}, DiagnosticSeverity::Hint);
        }
    }
}
} // namespace

void CheckRepeatedConversions(TSNode rootIfNode, const Scope* scope, DiagnosticContext& ctx)
{
    if (ts_node_is_null(rootIfNode))
    {
        return;
    }

    const auto ifNodes = CollectLadderIfNodes(rootIfNode);
    std::vector<ConversionOccurrence> occurrences;
    for (TSNode ifNode : ifNodes)
    {
        TSNode cond = GetIfCondition(ifNode);
        CollectConditionConversions(cond, scope, ctx, occurrences);
    }

    EmitRepeatedHints(occurrences, ctx);
}

std::vector<TSNode> FindOccurrencesInIfLadder(TSNode rootIf, std::string_view exprText,
                                              std::string_view sourceCode)
{
    std::vector<TSNode> results;
    if (ts_node_is_null(rootIf) || exprText.empty())
    {
        return results;
    }

    const TSQuery* query = parser::QueryRegistry::GetBinaryExpressionQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (!query || !cursor)
    {
        return results;
    }

    for (TSNode ifNode : CollectLadderIfNodes(rootIf))
    {
        TSNode cond = GetIfCondition(ifNode);
        if (ts_node_is_null(cond))
        {
            continue;
        }

        ts_query_cursor_exec(cursor, query, cond);
        TSQueryMatch match;
        while (ts_query_cursor_next_match(cursor, &match))
        {
            for (uint16_t i = 0; i < match.capture_count; ++i)
            {
                TSNode bin = match.captures[i].node;
                TSNode left = parser::GetChildByField(bin, parser::fields::Left);
                TSNode right = parser::GetChildByField(bin, parser::fields::Right);
                if (!ts_node_is_null(left) && GetNodeText(left, sourceCode) == exprText)
                {
                    results.push_back(left);
                }
                if (!ts_node_is_null(right) && GetNodeText(right, sourceCode) == exprText)
                {
                    results.push_back(right);
                }
            }
        }
    }
    return results;
}
} // namespace angel_lsp::analysis
