#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ReferenceCountingOperators.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{
static void CheckCallArgumentConversions(const FunctionSignature& sig, const IndexedOperatorSite& site,
                                         OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    if (!evalCtx.interest.hasImplConv && !evalCtx.interest.hasImplCast)
    {
        return;
    }
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    const size_t count = std::min<size_t>(site.argCount, sig.parameters.size());
    for (size_t i = 0; i < count; ++i)
    {
        const uint32_t argExprId = evalCtx.sites.argIds[site.argBegin + i];
        const std::string argType = ResolveIndexedExprType(argExprId, ctx);
        if (argType.empty() || argType == "null")
        {
            continue;
        }
        const auto& param = sig.parameters[i];
        const bool wantHandle = param.isHandle || IsHandleTypeView(param.typeName);
        if (wantHandle && IsDerivedFrom(CleanBaseTypeView(argType), CleanBaseTypeView(param.typeName), table))
        {
            continue;
        }
        const std::string_view method = wantHandle ? "opImplCast" : "opImplConv";
        const bool argConst = IsIndexedExprConst(argExprId, ctx);
        if (const auto match = FindConversionOperator({argType, param.typeName, method, argConst}, table))
        {
            const auto& argNode = evalCtx.sites.exprs[argExprId];
            RecordMatchedOperatorSymbol(*match, argNode.line, argNode.character, evalCtx);
        }
    }
}

static std::optional<Symbol> PickBestFromList(const std::vector<Symbol>& cands,
                                              const std::vector<std::string>& argTypes, const SymbolTable& table)
{
    if (cands.empty())
    {
        return std::nullopt;
    }
    if (cands.size() == 1)
    {
        return cands.front();
    }
    const auto match = ResolveBestOverload(cands, argTypes, table);
    if (match.bestCandidate)
    {
        return *match.bestCandidate;
    }
    return std::nullopt;
}

static std::optional<Symbol> ResolveFreeOrMemberCallTarget(const IndexedOperatorSite& site,
                                                           const std::vector<std::string>& argTypes,
                                                           OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    if (site.isMemberCall && site.exprId < evalCtx.sites.exprs.size())
    {
        const auto& calleeNode = evalCtx.sites.exprs[site.exprId];
        if (calleeNode.kind == IndexedExprKind::Member)
        {
            const std::string objType = ResolveIndexedExprType(calleeNode.left, ctx);
            if (!objType.empty())
            {
                const auto match = ResolveMemberOverload(
                    {objType, site.op, argTypes, IsIndexedExprConst(calleeNode.left, ctx)}, table);
                if (match.hasMatch)
                {
                    return match.symbol;
                }
            }
        }
        return std::nullopt;
    }

    if (ctx.scope && !ctx.scope->GetEnclosingClass().empty())
    {
        const auto m =
            ResolveMemberOverload({ctx.scope->GetEnclosingClass(), site.op, argTypes, ctx.scope->isConstMethod}, table);
        if (m.hasMatch)
        {
            return m.symbol;
        }
    }

    const std::string_view cleanName = utils::StripGlobalPrefix(site.op);
    if (const auto found = table.FindSymbolsPtr(cleanName))
    {
        std::vector<Symbol> cands;
        for (const auto& s : *found)
        {
            if (s.type == SymbolType::Function)
            {
                const auto arity = InspectFunctionArity(s.GetFunction());
                if (argTypes.size() >= arity.requiredParams && (arity.isVariadic || argTypes.size() <= arity.maxParams))
                {
                    cands.push_back(s);
                }
            }
        }
        if (const auto picked = PickBestFromList(cands, argTypes, table))
        {
            return picked;
        }
    }

    if (IsDeclaredTypeSymbol(cleanName, table))
    {
        const auto ctorMatch =
            ResolveMemberOverload({cleanName, utils::LastScopeSegment(cleanName), argTypes, false}, table);
        if (ctorMatch.hasMatch)
        {
            return ctorMatch.symbol;
        }
    }
    return std::nullopt;
}

void EvaluateOperatorCallSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const bool explicitOp = !site.op.empty() && rules::IsOperatorName(site.op);
    if (!explicitOp && !evalCtx.interest.hasCall && !evalCtx.interest.hasImplConv && !evalCtx.interest.hasImplCast)
    {
        return;
    }

    std::vector<std::string> argTypes;
    argTypes.reserve(site.argCount);
    for (uint32_t i = 0; i < site.argCount; ++i)
    {
        argTypes.push_back(ResolveIndexedExprType(evalCtx.sites.argIds[site.argBegin + i], ctx));
    }

    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    if (explicitOp)
    {
        if (const auto winner = ResolveFreeOrMemberCallTarget(site, argTypes, evalCtx, ctx))
        {
            RecordMatchedOperatorSymbol(*winner, site.line, site.character, evalCtx);
            CheckCallArgumentConversions(winner->GetFunction(), site, evalCtx, ctx);
        }
        return;
    }

    if (evalCtx.interest.hasCall && site.exprId < evalCtx.sites.exprs.size())
    {
        const std::string calleeType = ResolveIndexedExprType(site.exprId, ctx);
        if (!calleeType.empty() && !FindFuncdefSymbol(CleanBaseTypeView(calleeType), table).has_value())
        {
            const auto opCall =
                ResolveMemberOverload({calleeType, "opCall", argTypes, IsIndexedExprConst(site.exprId, ctx)}, table);
            if (opCall.hasMatch)
            {
                RecordMatchedOperatorSymbol(opCall.symbol, site.line, site.character, evalCtx);
                CheckCallArgumentConversions(opCall.symbol.GetFunction(), site, evalCtx, ctx);
                return;
            }
        }
    }

    if (evalCtx.interest.hasImplConv || evalCtx.interest.hasImplCast)
    {
        if (const auto winner = ResolveFreeOrMemberCallTarget(site, argTypes, evalCtx, ctx))
        {
            CheckCallArgumentConversions(winner->GetFunction(), site, evalCtx, ctx);
        }
    }
}
} // namespace angel_lsp::analysis
