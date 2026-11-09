#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks a returned value against the type its owner declares.
 * @param[in] site Indexed return statement.
 * @param[in] expected Cleaned declared return type; empty ends the check.
 * @param[in] skipPoison Whether a poisoned value type ends the check before the conversion rules run.
 * @param[in] scope Lexical scope the return statement lives in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckReturnedValue(const ReturnSiteFact& site, std::string_view expected, bool skipPoison,
                               const Scope* scope, DiagnosticContext& ctx)
{
    const BinaryOperandFact& value = site.value;
    if (expected == "void")
    {
        ctx.EmitAtRange(value.range, "as-err-void-return-value");
        return;
    }
    if (expected.empty())
    {
        return;
    }

    if (const auto dataTypeName = IsBareDataType(value.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(ConversionExpressionFact{value.bytes, value.range, value.text}, ctx, *dataTypeName);
        return;
    }
    if (IsFunctionReference(value.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(value.range, "as-err-no-implicit-conversion", {TrimString(value.text), expected});
        return;
    }

    const auto rawActual = ResolveExpressionType(value.bytes, ExpressionTypeContext(scope, ctx));
    if (skipPoison && ExpressionTypeCache::IsPoisonType(rawActual))
    {
        return;
    }
    const auto actual = CleanBaseType(rawActual);
    CheckFloatTruncation(value, {actual, CleanBaseType(expected)}, scope, ctx);
    if (!actual.empty() && actual != expected && !IsConvertible(actual, expected, ctx))
    {
        ctx.EmitAtRange(value.range, "as-err-no-implicit-conversion", {actual, expected});
    }
}

/**
 * @brief Checks a return inside a lambda against the return type of the funcdef it converts to.
 * @param[in] site Indexed return statement owned by a lambda.
 * @param[in] scope Lexical scope the return statement lives in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckLambdaReturnStatement(const ReturnSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    const auto target = FuncdefTargetOfLambda(site.lambdaTarget, ctx.request.symbolTable);
    if (!target)
    {
        return;
    }
    CheckReturnedValue(site, CleanBaseType(target->GetFuncdef().returnType), false, scope, ctx);
}

/**
 * @brief Rejects returning a parameter or local variable of the owning function by reference.
 * @param[in] site Indexed return statement owned by a function that returns by reference.
 * @param[in] scope Lexical scope the return statement lives in; nullptr ends the check.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckReturnReference(const ReturnSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    if (!scope)
    {
        return;
    }

    const std::string_view exprText = TrimString(site.value.text);
    const LocalDefinition* def = ResolveInScopeAt(scope, exprText, site.value.bytes.startByte);
    if (!def)
    {
        return;
    }

    const SourceRange& owner = site.ownerRange;
    const bool isInsideFunction = (def->startLine > owner.startLine && def->startLine < owner.endLine) ||
                                  (def->startLine == owner.startLine && def->startCharacter >= owner.startCharacter);
    if (!isInsideFunction)
    {
        return;
    }

    if (def->kind == LocalDefinitionKind::Parameter)
    {
        ctx.EmitAtRange(site.value.range, "as-err-cannot-return-param-ref", exprText);
    }
    else if (def->kind == LocalDefinitionKind::Variable)
    {
        ctx.EmitAtRange(site.value.range, "as-err-cannot-return-local-ref", exprText);
    }
}

/**
 * @brief Checks a return inside a function against its written return type.
 * @param[in] site Indexed return statement owned by a function with a written return type.
 * @param[in] scope Lexical scope the return statement lives in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckFuncDeclarationReturnStatement(const ReturnSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    if (site.returnsReference)
    {
        CheckReturnReference(site, scope, ctx);
    }
    CheckReturnedValue(site, CleanBaseType(site.returnType), true, scope, ctx);
}

void ProcessReturnSite(const ReturnSiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.statementRange, request);
    if (site.owner == ReturnOwnerKind::Lambda)
    {
        CheckLambdaReturnStatement(site, scope, ctx);
    }
    else if (site.owner == ReturnOwnerKind::Function)
    {
        CheckFuncDeclarationReturnStatement(site, scope, ctx);
    }
}

} // namespace angel_lsp::analysis
