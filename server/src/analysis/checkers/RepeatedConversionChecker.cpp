#include "analysis/checkers/RepeatedConversionChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsExtractableCandidate(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    return sym == gs.symMemberExpression || sym == gs.symIdentifier || sym == gs.symCallExpression ||
           sym == gs.symIndexExpression || sym == gs.symScopedIdentifier;
}

static void TryRecordOccurrence(TSNode operand, const ConversionTypePair& types, const DiagnosticContext& ctx,
                                std::vector<ConversionOccurrence>& out)
{
    if (!IsExtractableCandidate(operand))
    {
        return;
    }
    if (TypeHasOpImplConvTo(types.from, types.to, ctx.request.symbolTable))
    {
        out.push_back({operand, parser::GetNodeText(operand, ctx.request.sourceCode), types.from, types.to});
    }
}

static void InspectComparisonNode(TSNode binNode, const Scope* scope, DiagnosticContext& ctx,
                                  std::vector<ConversionOccurrence>& out)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto operands = ReadBinaryOperands(binNode, ctx.request.nodeIndex);
    if (!operands.Complete())
    {
        return;
    }
    const auto& [opNode, left, right] = operands;
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym != gs.tokEqEq && opSym != gs.tokBangEq)
    {
        return;
    }

    const ExpressionTypeContext exprCtx(scope, ctx);
    const std::string cleanLeft = CleanBaseType(ResolveExpressionType(left, exprCtx));
    const std::string cleanRight = CleanBaseType(ResolveExpressionType(right, exprCtx));
    if (cleanLeft.empty() || cleanRight.empty() || cleanLeft == cleanRight)
    {
        return;
    }

    TryRecordOccurrence(left, {cleanLeft, cleanRight}, ctx, out);
    TryRecordOccurrence(right, {cleanRight, cleanLeft}, ctx, out);
}

/// Visits only indexed binary candidates contained in the condition's byte interval.
static void CollectConditionConversions(TSNode condition, const Scope* scope, DiagnosticContext& ctx,
                                        std::vector<ConversionOccurrence>& out)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto candidates = ctx.request.nodeIndex->Nodes(gs.symBinaryExpression);
    const auto start = ts_node_start_byte(condition);
    const auto end = ts_node_end_byte(condition);
    const auto first = std::ranges::lower_bound(candidates, start, {}, ts_node_start_byte);
    const auto last = std::ranges::lower_bound(first, candidates.end(), end, {}, ts_node_start_byte);
    for (auto it = first; it != last; ++it)
    {
        if (ts_node_end_byte(*it) <= end)
        {
            InspectComparisonNode(*it, scope, ctx, out);
        }
    }
}

std::vector<TSNode> CollectIfLadderConditions(TSNode rootIf, const NodeIndex* index)
{
    std::vector<TSNode> conditions;
    const auto& gs = parser::GetGrammarSymbols();
    for (TSNode current = rootIf; !ts_node_is_null(current) && ts_node_symbol(current) == gs.symIfStatement;)
    {
        const SyntaxOperands syntax(current, index);
        const TSNode condition = syntax.FieldOrNamed(gs.fieldCondition, 0);
        if (!ts_node_is_null(condition))
        {
            conditions.push_back(condition);
        }
        current = syntax.Field(gs.fieldAlternative);
    }
    return conditions;
}

static void EmitRepeatedHints(const std::vector<ConversionOccurrence>& occurrences, DiagnosticContext& ctx)
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

void CheckRepeatedConversions(TSNode rootIfNode, const Scope* scope, DiagnosticContext& ctx)
{
    if (!ctx.request.nodeIndex)
    {
        return;
    }

    std::vector<ConversionOccurrence> occurrences;
    for (TSNode condition : CollectIfLadderConditions(rootIfNode, ctx.request.nodeIndex))
    {
        CollectConditionConversions(condition, scope, ctx, occurrences);
    }
    EmitRepeatedHints(occurrences, ctx);
}
} // namespace angel_lsp::analysis
