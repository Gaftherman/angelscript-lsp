#include "analysis/checkers/RepeatedConversionChecker.h"

#include "analysis/checkers/BinaryOperatorHelpers.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/OperandTypePair.h"
#include "analysis/types/TypeSanitization.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace angel_lsp::analysis
{

static void TryRecordOccurrence(const BinaryOperandFact& operand, const OperandTypePair& types,
                                const DiagnosticContext& ctx, std::vector<ConversionOccurrence>& out)
{
    if (!operand.isExtractableCandidate)
    {
        return;
    }
    if (TypeHasOpImplConvTo(types.left, types.right, ctx.request.symbolTable))
    {
        out.push_back({operand.range, operand.text, types});
    }
}

static void InspectComparisonNode(const BinaryExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx,
                                  std::vector<ConversionOccurrence>& out)
{
    if (fact.opClass != BinaryOperatorClass::Equality)
    {
        return;
    }

    const ExpressionTypeContext exprCtx(scope, ctx);
    const auto cleanLeft = CleanBaseType(ResolveExpressionType(fact.left.bytes, exprCtx));
    const auto cleanRight = CleanBaseType(ResolveExpressionType(fact.right.bytes, exprCtx));
    if (cleanLeft.empty() || cleanRight.empty() || cleanLeft == cleanRight)
    {
        return;
    }

    TryRecordOccurrence(fact.left, MakeOperandTypePair(cleanLeft, cleanRight), ctx, out);
    TryRecordOccurrence(fact.right, MakeOperandTypePair(cleanRight, cleanLeft), ctx, out);
}

static void EmitRepeatedHints(const std::vector<ConversionOccurrence>& occurrences, DiagnosticContext& ctx)
{
    if (occurrences.size() < 2)
    {
        return;
    }

    std::unordered_map<std::string_view, size_t> counts;
    for (const auto& occ : occurrences)
    {
        ++counts[occ.exprText];
    }

    for (const auto& occ : occurrences)
    {
        if (counts[occ.exprText] >= 2)
        {
            ctx.EmitAtRange(occ.range, diagnostics::codes::RepeatedConversion,
                            {occ.exprText, occ.types.left, occ.types.right}, DiagnosticSeverity::Hint);
        }
    }
}

/**
 * @brief Checks for repeated implicit conversions in conditional statement ladders.
 * @param[in] ladder Root if statement and its else-if conditions.
 * @param[in] facts Binary expression facts that hold the comparisons inside the conditions.
 * @param[in] scope Lexical scope of the conditional statement.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckRepeatedConversions(const IfLadderFact& ladder, const BinaryExpressionFacts& facts, const Scope* scope,
                              DiagnosticContext& ctx)
{
    std::vector<ConversionOccurrence> occurrences;
    const auto conditionEnd = ladder.conditionBegin + ladder.conditionCount;
    for (uint32_t i = ladder.conditionBegin; i < conditionEnd && i < facts.ladderConditions.size(); ++i)
    {
        const auto& span = facts.ladderConditions[i];
        for (const auto* fact : facts.FindWithin(span))
        {
            if (fact)
            {
                InspectComparisonNode(*fact, scope, ctx, occurrences);
            }
        }
    }
    EmitRepeatedHints(occurrences, ctx);
}

} // namespace angel_lsp::analysis
