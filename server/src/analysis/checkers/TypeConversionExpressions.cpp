#include "analysis/checkers/BinaryOperatorChecker.h"
#include "analysis/checkers/ComparisonOperatorChecker.h"
#include "analysis/checkers/HandleComparisonChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

static std::string FormatOpIndexCall(std::string_view owner, const std::vector<std::string>& argTypes)
{
    std::string out(owner);
    out += "::opIndex(";
    for (size_t i = 0; i < argTypes.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += argTypes[i].empty() ? "?" : argTypes[i];
    }
    return out + ")";
}

static size_t CollectMatchingOpIndexOverloads(std::string_view owner, std::string_view normalizedObj, uint32_t argCount,
                                              const SymbolTable& table, std::vector<Symbol>& outOverloads)
{
    TemplateBinding binding;
    if (normalizedObj.find('<') != std::string::npos)
        binding = BindTemplateArguments(normalizedObj, table);

    size_t totalOpIndex = 0;
    for (const auto& cls : GetInheritedTypeHierarchy(owner, table))
    {
        const auto found = table.FindMemberSymbolPtr(cls, "opIndex");
        if (!found)
            continue;
        for (const auto& sym : *found)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
                continue;
            ++totalOpIndex;
            const auto arity = InspectFunctionArity(sym.GetFunction());
            if (argCount < arity.requiredParams || (!arity.isVariadic && argCount > arity.maxParams))
                continue;
            Symbol copy = sym;
            if (binding.usable)
            {
                for (auto& param : copy.GetFunction().parameters)
                    param.typeName = SubstituteTemplateParameters(param.typeName, binding);
            }
            outOverloads.push_back(std::move(copy));
        }
    }
    return totalOpIndex;
}

static void ValidateOpIndexInvocation(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                      const Scope* scope, DiagnosticContext& ctx)
{
    if (site.hasSyntaxError || site.objectBytes.endByte == 0 || site.argCount == 0 || !request.expressionSites ||
        site.argBegin + site.argCount > request.expressionSites->indexArgs.size())
        return;

    const ExpressionTypeContext exprCtx(scope, ctx);
    if (IsBareDataType(site.objectBytes, exprCtx) || IsFunctionReference(site.objectBytes, exprCtx))
        return;

    const std::string rawObjType = ResolveExpressionType(site.objectBytes, exprCtx);
    if (rawObjType.empty() || ExpressionTypeCache::IsPoisonType(rawObjType))
        return;

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayTypeName = ctx.request.GetEffectiveArrayTypeName();
    const std::string normalizedObj = UnwrapTypedef(NormalizeType(rawObjType, arrayTypeName), table);
    if (normalizedObj.empty() || normalizedObj == "auto" || normalizedObj == "?" || normalizedObj == "any" ||
        ctx.request.IsRegisteredSymbol(normalizedObj))
        return;

    if (IsPrimitiveTypeName(normalizedObj) || ResolvesToEnum(normalizedObj, table) ||
        FindFuncdef(normalizedObj, table).has_value())
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::TypeNotIndexable, normalizedObj);
        return;
    }

    std::string owner = MemberOwnerType(normalizedObj, arrayTypeName);
    if (owner.empty())
        owner = CleanBaseType(normalizedObj);
    if (ctx.request.IsRegisteredSymbol(owner) || !HierarchyIsFullyVisible(owner, table))
        return;

    std::vector<Symbol> overloads;
    if (CollectMatchingOpIndexOverloads(owner, normalizedObj, site.argCount, table, overloads) == 0)
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::TypeNotIndexable, owner);
        return;
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(site.argCount);
    bool allResolved = true;
    for (uint32_t i = 0; i < site.argCount; ++i)
    {
        std::string argType =
            ResolveExpressionType(request.expressionSites->indexArgs[site.argBegin + i].bytes, exprCtx);
        const std::string_view cleanArg = CleanBaseTypeView(argType);
        if (argType.empty() || ExpressionTypeCache::IsPoisonType(argType) || cleanArg == "auto" || cleanArg == "?" ||
            ctx.request.IsRegisteredSymbol(cleanArg))
            allResolved = false;
        argTypes.push_back(std::move(argType));
    }

    if (overloads.empty())
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::CallNoMatchingSignature, FormatOpIndexCall(owner, argTypes));
        return;
    }
    if (!allResolved)
        return;

    std::vector<const Symbol*> matchingArity;
    matchingArity.reserve(overloads.size());
    for (const auto& s : overloads)
        matchingArity.push_back(&s);

    const OverloadMatchResult match = ResolveBestOverload(matchingArity, argTypes, table);
    // Const and non-const opIndex pairs share their parameters, so several viable candidates without a
    // single best one is a valid container, not a mismatch.
    if (match.viableCandidates.empty())
        ctx.EmitAtRange(site.range, diagnostics::codes::CallNoMatchingSignature, FormatOpIndexCall(owner, argTypes));
}

/**
 * @brief Reports an index expression that reads a write-only property or calls an invalid opIndex.
 * @param[in] site Index-expression fact.
 * @param[in] request Check request giving access to the scope tree.
 * @param[in,out] ctx Diagnostic sink.
 */
void ProcessIndexExpressionSite(const IndexExpressionSiteFact& site, const TypeConversionCheckRequest& request,
                                DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.range, request);
    const PropertyAccessInfo pInfo = InspectPropertyAccess(site.objectProperty, scope, ctx);
    if (pInfo.isProperty && pInfo.isIndexed)
    {
        if (!site.isWriteOrMutation && !pInfo.hasGet && pInfo.hasSet)
        {
            ctx.EmitAtRange(site.range, "as-err-write-only-property", pInfo.propName);
        }
        return;
    }

    ValidateOpIndexInvocation(site, request, scope, ctx);
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
