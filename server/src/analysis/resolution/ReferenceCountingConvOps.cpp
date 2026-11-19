#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ReferenceCountingOperators.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{
std::optional<Symbol> FindConversionOperator(const ConversionLookupQuery& query, const SymbolTable& symbolTable)
{
    if (query.srcType.empty() || query.targetType.empty())
    {
        return std::nullopt;
    }
    const std::string_view arrayType = symbolTable.GetDefaultArrayType();
    const std::string normSrc = UnwrapTypedef(NormalizeType(query.srcType, arrayType), symbolTable);
    const std::string normTarget = UnwrapTypedef(NormalizeType(query.targetType, arrayType), symbolTable);
    const std::string_view cleanSrc = CleanBaseTypeView(normSrc, arrayType);
    const std::string_view cleanTarget = CleanBaseTypeView(normTarget, arrayType);
    if (cleanSrc.empty() || cleanTarget.empty() || IsPrimitiveTypeName(cleanSrc) || IsSameType(normSrc, normTarget))
    {
        return std::nullopt;
    }

    const std::string owner = MemberOwnerType(normSrc, arrayType);
    if (owner.empty())
    {
        return std::nullopt;
    }
    const TemplateBinding binding =
        (HasTemplateArgumentsView(normSrc)) ? BindTemplateArguments(normSrc, symbolTable) : TemplateBinding{};

    std::optional<Symbol> wideningCandidate;
    for (const auto& cls : GetInheritedTypeHierarchy(owner, symbolTable))
    {
        const auto found = symbolTable.FindMemberSymbolPtr(cls, query.methodName);
        if (!found)
        {
            continue;
        }
        for (const auto& sym : *found)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            const auto& fn = sym.GetFunction();
            if (fn.modifiers.isDelete || InspectFunctionArity(fn).requiredParams > 0)
            {
                continue;
            }
            if (query.isSrcConst && !fn.modifiers.isConst)
            {
                continue;
            }
            std::string ret = fn.returnType;
            if (binding.usable)
            {
                ret = SubstituteTemplateParameters(ret, binding);
            }
            const std::string normRet = UnwrapTypedef(NormalizeType(ret, arrayType), symbolTable);
            if (IsSameType(normRet, normTarget) || IsSameType(CleanBaseTypeView(normRet, arrayType), cleanTarget))
            {
                return sym;
            }
            if (!wideningCandidate && IsNumericPrimitive(normRet) && IsNumericPrimitive(cleanTarget) &&
                IsPrimitiveWidening(normRet, cleanTarget))
            {
                wideningCandidate = sym;
            }
        }
    }
    return wideningCandidate;
}

static void EvaluateCastSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const std::string srcType = ResolveIndexedExprType(site.exprId, ctx);
    if (srcType.empty() || site.targetType.empty())
    {
        return;
    }
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    const bool isConst = IsIndexedExprConst(site.exprId, ctx);
    if (site.op == "cast")
    {
        if (const auto match = FindConversionOperator({srcType, site.targetType, "opCast", isConst}, table))
        {
            RecordMatchedOperatorSymbol(*match, site.line, site.character, evalCtx);
            return;
        }
        const std::string_view cleanSrc = CleanBaseTypeView(srcType, table.GetDefaultArrayType());
        const std::string_view cleanDst = CleanBaseTypeView(site.targetType, table.GetDefaultArrayType());
        if (!IsDerivedFrom(cleanSrc, cleanDst, table) && !IsDerivedFrom(cleanDst, cleanSrc, table))
        {
            if (const auto match = FindConversionOperator({srcType, site.targetType, "opImplCast", isConst}, table))
            {
                RecordMatchedOperatorSymbol(*match, site.line, site.character, evalCtx);
            }
        }
        return;
    }
    if (const auto match = FindConversionOperator({srcType, site.targetType, "opConv", isConst}, table))
    {
        RecordMatchedOperatorSymbol(*match, site.line, site.character, evalCtx);
    }
    else if (const auto impl = FindConversionOperator({srcType, site.targetType, "opImplConv", isConst}, table))
    {
        RecordMatchedOperatorSymbol(*impl, site.line, site.character, evalCtx);
    }
}

static void EvaluateForeachSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    if (!evalCtx.interest.hasForeach)
    {
        return;
    }
    const std::string colType = ResolveIndexedExprType(site.exprId, ctx);
    if (colType.empty())
    {
        return;
    }
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    const bool isConst = IsIndexedExprConst(site.exprId, ctx);
    const auto beginMatch = ResolveMemberOverload({colType, "opForBegin", {}, isConst}, table);
    std::string iterType;
    if (beginMatch.hasMatch)
    {
        RecordMatchedOperatorSymbol(beginMatch.symbol, site.line, site.character, evalCtx);
        iterType = SubstituteMemberReturnType(beginMatch.symbol.GetFunction().returnType, colType, table);
    }

    const std::vector<std::string> iterArgs =
        iterType.empty() ? std::vector<std::string>{} : std::vector<std::string>{iterType};
    for (const std::string_view m : {"opForEnd", "opForNext"})
    {
        const auto match = ResolveMemberOverload({colType, m, iterArgs, isConst}, table);
        if (match.hasMatch)
        {
            RecordMatchedOperatorSymbol(match.symbol, site.line, site.character, evalCtx);
        }
    }

    if (site.argCount <= 1)
    {
        auto valMatch = ResolveMemberOverload({colType, "opForValue", iterArgs, isConst}, table);
        if (!valMatch.hasMatch)
        {
            valMatch = ResolveMemberOverload({colType, "opForValue0", iterArgs, isConst}, table);
        }
        if (valMatch.hasMatch)
        {
            RecordMatchedOperatorSymbol(valMatch.symbol, site.line, site.character, evalCtx);
        }
        return;
    }
    for (uint32_t i = 0; i < site.argCount; ++i)
    {
        const std::string name = "opForValue" + std::to_string(i);
        const auto valMatch = ResolveMemberOverload({colType, name, iterArgs, isConst}, table);
        if (valMatch.hasMatch)
        {
            RecordMatchedOperatorSymbol(valMatch.symbol, site.line, site.character, evalCtx);
        }
    }
}

static void EvaluateImplicitConversionSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx,
                                           ExprTypeEvalContext& ctx)
{
    if (!evalCtx.interest.hasImplConv && !evalCtx.interest.hasImplCast)
    {
        return;
    }
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    if (site.kind == OperatorSiteKind::Assignment)
    {
        if (site.op != "=" || site.leftIsHandle)
        {
            return;
        }
        const std::string leftType = ResolveIndexedExprType(site.exprId, ctx);
        const std::string rightType = ResolveIndexedExprType(site.rhsExprId, ctx);
        if (leftType.empty() || rightType.empty())
        {
            return;
        }
        const bool isConst = IsIndexedExprConst(site.rhsExprId, ctx);
        if (const auto match = FindConversionOperator({rightType, leftType, "opImplConv", isConst}, table))
        {
            const auto& rhsNode = evalCtx.sites.exprs[site.rhsExprId];
            RecordMatchedOperatorSymbol(*match, rhsNode.line, rhsNode.character, evalCtx);
        }
        return;
    }

    std::string targetType = site.targetType;
    if (targetType.empty() && ctx.scope)
    {
        targetType = std::string(ctx.scope->GetReturnType());
    }
    if (targetType.empty() || parser::primitives::IsVoidType(targetType) || parser::primitives::IsAutoType(targetType))
    {
        return;
    }
    const std::string srcType = ResolveIndexedExprType(site.exprId, ctx);
    if (srcType.empty() || parser::primitives::IsNullType(srcType))
    {
        return;
    }
    if (site.isCondition && IsHandleTypeView(srcType))
    {
        return;
    }
    const bool wantHandle = !site.isCondition && (site.isTargetHandle || IsHandleTypeView(targetType));
    if (wantHandle && IsDerivedFrom(CleanBaseTypeView(srcType), CleanBaseTypeView(targetType), table))
    {
        return;
    }
    const std::string_view method = wantHandle ? "opImplCast" : "opImplConv";
    if (const auto match =
            FindConversionOperator({srcType, targetType, method, IsIndexedExprConst(site.exprId, ctx)}, table))
    {
        RecordMatchedOperatorSymbol(*match, site.line, site.character, evalCtx);
    }
}

void EvaluateCallForeachOrConversionSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx)
{
    const Scope* scope = FindInnermostScope(evalCtx.rootScope, site.scopeLine, site.scopeCharacter);
    if (!scope)
    {
        scope = evalCtx.rootScope;
    }
    ExprTypeEvalContext exprCtx{scope, evalCtx.sites, evalCtx.batch.request.symbolTable, evalCtx.exprMemo,
                                evalCtx.exprState};
    switch (site.kind)
    {
    case OperatorSiteKind::Cast:
        if (evalCtx.interest.hasCast)
        {
            EvaluateCastSite(site, evalCtx, exprCtx);
        }
        break;
    case OperatorSiteKind::Call:
        EvaluateOperatorCallSite(site, evalCtx, exprCtx);
        break;
    case OperatorSiteKind::Foreach:
        EvaluateForeachSite(site, evalCtx, exprCtx);
        break;
    case OperatorSiteKind::Assignment:
    case OperatorSiteKind::ImplicitConversion:
        EvaluateImplicitConversionSite(site, evalCtx, exprCtx);
        break;
    default:
        break;
    }
}
} // namespace angel_lsp::analysis
