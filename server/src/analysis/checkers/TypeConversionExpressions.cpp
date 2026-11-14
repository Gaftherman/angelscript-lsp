#include "analysis/checkers/BinaryOperatorChecker.h"
#include "analysis/checkers/ComparisonOperatorChecker.h"
#include "analysis/checkers/HandleComparisonChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/TypeSanitization.h"

#include <string_view>

namespace angel_lsp::analysis
{

void ProcessBinarySite(const BinarySiteFact& site, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const BinaryExpressionFact* fact = request.binaryFacts->Find(site.bytes);
    if (!fact)
    {
        return;
    }
    const Scope* scope = ResolveNodeScope(site.range, request);

    if (fact->opClass == BinaryOperatorClass::Identity)
    {
        CheckHandleComparison(*fact, scope, ctx);
        return;
    }

    if (fact->opClass == BinaryOperatorClass::Equality || fact->opClass == BinaryOperatorClass::Relational)
    {
        CheckSignedUnsignedComparison(*fact, scope, ctx);
        CheckHandleComparison(*fact, scope, ctx);
        CheckComparisonOperatorCompatibility(*fact, scope, ctx);
        return;
    }

    CheckBinaryOperatorCompatibility(*fact, scope, ctx);
}

static bool CheckUnaryInvalidOperand(const UnaryPostfixSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    if (!site.hasOperand)
    {
        return false;
    }
    const ExpressionTypeContext exprCtx(scope, ctx);
    if (const auto dt = IsBareDataType(site.operandBytes, exprCtx))
    {
        EmitExpressionIsDataType(ConversionExpressionFact{site.operandBytes, site.operandRange, site.operandText}, ctx,
                                 *dt);
        return true;
    }
    if (site.isAddressOfHandle)
    {
        return false;
    }
    if (IsFunctionReference(site.operandBytes, exprCtx))
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::IllegalOperation);
        return true;
    }
    return false;
}

void ProcessUnaryOrPostfixSite(const UnaryPostfixSiteFact& site, const TypeConversionCheckRequest& request,
                               DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.range, request);
    if (CheckUnaryInvalidOperand(site, scope, ctx))
    {
        return;
    }

    if (!site.isIncrementOrDecrement || !site.hasOperand)
    {
        return;
    }

    const PropertyAccessInfo pInfo = InspectPropertyAccess(site.operandProperty, scope, ctx);
    if (site.operandIsIndexed)
    {
        if (pInfo.isProperty && pInfo.isIndexed)
        {
            ctx.EmitAtRange(site.range, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
        }
    }
    else if (pInfo.isProperty)
    {
        ctx.EmitAtRange(site.range, "as-err-inc-dec-on-virtual-prop", pInfo.propName);
    }
}

/**
 * @brief Reports a member access that reads a write-only property.
 * @param[in] site Member-expression fact; call callees and write or mutation sites are skipped.
 * @param[in] request Check request giving access to the scope tree.
 * @param[in,out] ctx Diagnostic sink that receives as-err-write-only-property.
 */
void ProcessMemberExpressionSite(const MemberExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                 DiagnosticContext& ctx)
{
    if (site.isCallCallee || site.isWriteOrMutation)
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(site.range, request);
    const PropertyAccessInfo pInfo = InspectPropertyAccess(site.propertyAccess, scope, ctx);
    if (pInfo.isProperty && !pInfo.hasGet && pInfo.hasSet)
    {
        ctx.EmitAtRange(site.range, "as-err-write-only-property", pInfo.propName);
    }
}

/**
 * @brief Reports an index expression that reads through a write-only indexed property.
 * @param[in] site Index-expression fact; write or mutation sites are skipped.
 * @param[in] request Check request giving access to the scope tree.
 * @param[in,out] ctx Diagnostic sink that receives as-err-write-only-property.
 */
void ProcessIndexExpressionSite(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx)
{
    if (site.isWriteOrMutation)
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(site.range, request);
    const PropertyAccessInfo pInfo = InspectPropertyAccess(site.objectProperty, scope, ctx);
    if (pInfo.isProperty && pInfo.isIndexed && !pInfo.hasGet && pInfo.hasSet)
    {
        ctx.EmitAtRange(site.range, "as-err-write-only-property", pInfo.propName);
    }
}

static bool CheckCastVisibilityGuards(const ConversionTypePair& types, bool sourceIsScalar, bool targetIsScalar,
                                      const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!targetIsScalar && !FindTypeDeclaration(types.to, table).found)
    {
        return false;
    }
    if (!sourceIsScalar && !FindTypeDeclaration(types.from, table).found)
    {
        return false;
    }
    if (ctx.request.IsRegisteredSymbol(types.from) || ctx.request.IsRegisteredSymbol(types.to))
    {
        return false;
    }
    return true;
}

void ProcessCastSite(const CastExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                     DiagnosticContext& ctx)
{
    const auto targetName = CleanBaseType(site.targetType);
    if (!site.hasValue || targetName.empty())
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(site.range, request);
    const SymbolTable& table = ctx.request.symbolTable;
    const bool targetIsScalar = IsScalarCastTarget(targetName, table);

    const ExpressionType source = ResolveValueType(site.value, scope, ctx);
    if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetName))
    {
        return;
    }

    const bool sourceIsScalar = IsScalarCastTarget(source.baseName, table);
    if (!CheckCastVisibilityGuards({source.baseName, targetName}, sourceIsScalar, targetIsScalar, ctx))
    {
        return;
    }

    if (AreHierarchyRelated(source.baseName, targetName, table))
    {
        return;
    }

    if (!sourceIsScalar && !targetIsScalar)
    {
        return;
    }

    if (DeclaresAnyCastOperator(source.baseName, table) || DeclaresAnyCastOperator(targetName, table))
    {
        return;
    }

    ctx.EmitAtRange(site.range, "as-err-invalid-cast", {source.baseName, targetName});
}

} // namespace angel_lsp::analysis
