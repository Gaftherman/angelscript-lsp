#include "analysis/checkers/RepeatedConversionChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>
#include <variant>

namespace angel_lsp::analysis
{

/**
 * @brief Reports a condition operand that is not boolean, or hints at its implicit bool conversion.
 * @param[in] operand Indexed leaf operand of the condition.
 * @param[in] operandType Cleaned base type of the operand.
 * @param[in,out] ctx Diagnostic context.
 */
static void ValidateConditionOperand(const ConversionExpressionFact& operand, std::string_view operandType,
                                     DiagnosticContext& ctx)
{
    const bool isKnown = IsKnownType(operandType, ctx);

    if (isKnown && !IsTruthyCondition(operandType, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(operand.range, "as-err-condition-not-boolean", operandType);
        return;
    }

    if (!ctx.request.diagnostics || !ctx.request.diagnostics->reportBoolConversion ||
        ctx.request.BoolConversionMode() != 0)
    {
        return;
    }

    if (const std::string_view conversion = BoolConversionOperator(operandType, ctx.request.symbolTable);
        !conversion.empty())
    {
        ctx.EmitAtRange(operand.range, "as-hint-bool-conversion", {operandType, conversion}, DiagnosticSeverity::Hint);
    }
}

/**
 * @brief Validates every leaf operand of a condition against the boolean conversion rules.
 * @param[in] facts Document conversion sites owning the operand slice.
 * @param[in] site Condition site whose operand slice is visited.
 * @param[in] scope Lexical scope the condition is evaluated in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckBooleanOperands(const ConversionSiteFacts& facts, const ConditionSiteFact& site, const Scope* scope,
                                 DiagnosticContext& ctx)
{
    for (uint32_t i = 0; i < site.operandCount; ++i)
    {
        const ConversionExpressionFact& operand = facts.booleanOperands[site.operandBegin + i];
        const auto operandType = CleanBaseType(ResolveExpressionType(operand.bytes, ExpressionTypeContext(scope, ctx)));

        if (operandType.empty() || operandType == "auto" || operandType == "void")
        {
            continue;
        }

        ValidateConditionOperand(operand, operandType, ctx);
    }
}

/**
 * @brief Rejects a handle condition whose class converts to bool through `opImplConv`.
 * @param[in] unwrapped Condition with parentheses removed; absent facts end the check.
 * @param[in] scope Lexical scope the condition is evaluated in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckRefTypeBoolConversion(const ConversionExpressionFact& unwrapped, const Scope* scope,
                                       DiagnosticContext& ctx)
{
    if (!unwrapped.Present())
    {
        return;
    }

    bool isHandleTarget = false;
    if (scope)
    {
        const LocalDefinition* def = ResolveInScopeAt(scope, unwrapped.text, unwrapped.bytes.startByte);
        if (def)
        {
            isHandleTarget = def->isHandleType || IsHandleTypeView(def->typeName);
        }
    }

    const auto condType = ResolveExpressionType(unwrapped.bytes, ExpressionTypeContext(scope, ctx));
    if (!isHandleTarget && !IsHandleTypeView(condType))
    {
        return;
    }

    const auto baseClass = CleanBaseType(condType);
    auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(baseClass, "opImplConv");
    if (!opSyms)
    {
        return;
    }

    for (const auto& sym : *opSyms)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            if (CleanBaseType(sym.GetFunction().returnType) == "bool")
            {
                ctx.EmitAtRange(unwrapped.range, "as-err-ref-type-bool-conv-disallowed", {baseClass, "bool"});
                break;
            }
        }
    }
}

void ProcessConditionSite(const ConditionSiteFact& site, const TypeConversionCheckRequest& request,
                          DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.statementRange, request);
    CheckBooleanOperands(*request.conversionSites, site, scope, ctx);

    if (!site.isTernary)
    {
        CheckRefTypeBoolConversion(site.unwrapped, scope, ctx);
    }

    if (site.isRootIf)
    {
        if (const IfLadderFact* ladder = request.binaryFacts->FindLadder(site.bytes))
        {
            CheckRepeatedConversions(*ladder, *request.binaryFacts, scope, ctx);
        }
    }
}

} // namespace angel_lsp::analysis
