#include "analysis/resolution/ReferenceCountingOperators.h"

#include "analysis/resolution/ReferenceCountingExprType.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
static OperatorTargetInterest ComputeTargetInterest(const BatchReferenceContext& ctx)
{
    OperatorTargetInterest interest;
    for (const auto& target : ctx.targets)
    {
        if (!target.isFunction || !rules::IsOperatorName(target.targetName))
        {
            continue;
        }
        interest.hasAnyOperator = true;
        const std::string_view name = target.targetName;
        if (name == "opImplConv")
        {
            interest.hasImplConv = true;
            interest.hasCast = true;
        }
        else if (name == "opImplCast")
        {
            interest.hasImplCast = true;
            interest.hasCast = true;
        }
        else if (name == "opCast" || name == "opConv")
        {
            interest.hasCast = true;
        }
        else if (name == "opCall")
        {
            interest.hasCall = true;
        }
        else if (name == "opIndex")
        {
            interest.hasIndex = true;
        }
        else if (name.starts_with("opFor"))
        {
            interest.hasForeach = true;
        }
        else if (name == "opNeg" || name == "opCom" || name == "opPreInc" || name == "opPreDec" ||
                 name == "opPostInc" || name == "opPostDec")
        {
            interest.hasUnary = true;
        }
        else if (name == "opAssign" || name == "opHndlAssign" || name.ends_with("Assign"))
        {
            interest.hasAssign = true;
        }
        else
        {
            interest.hasBinary = true;
        }
    }
    return interest;
}

/**
 * @brief Maps a written assignment token to the corresponding overload method.
 * @param[in] op Spelling captured for an assignment site, including its handle marker where written.
 * @return Overload method name, or an empty view when the token has no mapping.
 */
static std::string_view AssignmentOpMethod(std::string_view op) noexcept
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 14> k_map = {{
        {"=", "opAssign"},
        {"@=", "opHndlAssign"},
        {"+=", "opAddAssign"},
        {"-=", "opSubAssign"},
        {"*=", "opMulAssign"},
        {"/=", "opDivAssign"},
        {"%=", "opModAssign"},
        {"**=", "opPowAssign"},
        {"&=", "opAndAssign"},
        {"|=", "opOrAssign"},
        {"^=", "opXorAssign"},
        {"<<=", "opShlAssign"},
        {">>=", "opShrAssign"},
        {">>>=", "opUShrAssign"},
    }};
    for (const auto& [tok, method] : k_map)
    {
        if (tok == op)
        {
            return method;
        }
    }
    return {};
}

static std::string_view UnaryOpMethod(std::string_view op, bool isPostfix) noexcept
{
    if (op == "-")
        return "opNeg";
    if (op == "~")
        return "opCom";
    if (op == "++")
        return isPostfix ? "opPostInc" : "opPreInc";
    if (op == "--")
        return isPostfix ? "opPostDec" : "opPreDec";
    return {};
}

static void EvaluateAssignmentSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx,
                                   ExprTypeEvalContext& ctx)
{
    const std::string leftType = ResolveIndexedExprType(site.exprId, ctx);
    const std::string rightType = ResolveIndexedExprType(site.rhsExprId, ctx);
    if (leftType.empty() || rightType.empty())
    {
        return;
    }
    if (site.op == "=" &&
        (site.leftIsHandle || (parser::primitives::IsNullType(rightType) && IsHandleTypeView(leftType))))
    {
        return;
    }
    const std::string_view method = AssignmentOpMethod(site.op);
    if (method.empty())
    {
        return;
    }
    const auto match =
        ResolveSingleArgOverload({leftType, method, {}, false}, rightType, evalCtx.batch.request.symbolTable);
    if (match.hasMatch)
    {
        RecordMatchedOperatorSymbol(match.symbol, site.line, site.character, evalCtx);
    }
}

/**
 * @brief Records a unary overload use when a nonprimitive operand selects a member.
 * @param[in] site Operator token and operand fact, including prefix or postfix placement.
 * @param[in,out] evalCtx Reference batch receiving the use at the operator position.
 * @param[in,out] ctx Lexical type and constness lookup for the operand.
 */
static void EvaluateUnarySite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const std::string_view method = UnaryOpMethod(site.op, site.isPostfix);
    if (method.empty())
    {
        return;
    }
    const std::string opType = ResolveIndexedExprType(site.exprId, ctx);
    if (opType.empty() || IsPrimitiveTypeName(CleanBaseTypeView(opType)))
    {
        return;
    }
    const bool isConst = IsIndexedExprConst(site.exprId, ctx);
    const auto match = ResolveMemberOverload({opType, method, {}, isConst}, evalCtx.batch.request.symbolTable);
    if (match.hasMatch)
    {
        RecordMatchedOperatorSymbol(match.symbol, site.line, site.character, evalCtx);
    }
}

/**
 * @brief Records the subscript overload selected for the receiver and its ordered indices.
 * @param[in] site Subscript receiver and argument fact IDs.
 * @param[in,out] evalCtx Operator facts and reference batch receiving the use.
 * @param[in,out] ctx Expression context used to infer argument types and receiver constness.
 */
static void EvaluateIndexSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const std::string objType = ResolveIndexedExprType(site.exprId, ctx);
    if (objType.empty())
    {
        return;
    }
    std::vector<std::string> argTypes;
    argTypes.reserve(site.argCount);
    for (uint32_t i = 0; i < site.argCount; ++i)
    {
        argTypes.push_back(ResolveIndexedExprType(evalCtx.sites.argIds[site.argBegin + i], ctx));
    }
    const bool isConst = IsIndexedExprConst(site.exprId, ctx);
    const auto match =
        ResolveMemberOverload({objType, "opIndex", argTypes, isConst}, evalCtx.batch.request.symbolTable);
    if (match.hasMatch)
    {
        RecordMatchedOperatorSymbol(match.symbol, site.line, site.character, evalCtx);
    }
}

void RecordMatchedOperatorSymbol(const Symbol& winner, uint32_t line, uint32_t col, const OperatorEvalContext& evalCtx)
{
    const BatchReferenceContext& ctx = evalCtx.batch;
    const auto it = ctx.targetsByName.find(winner.name);
    if (it == ctx.targetsByName.end())
    {
        return;
    }
    const uint64_t pos = (static_cast<uint64_t>(line) << 32) | col;
    for (const size_t targetIdx : it->second)
    {
        auto& target = ctx.targets[targetIdx];
        if (!target.isFunction)
        {
            continue;
        }
        for (const auto& sym : target.group)
        {
            if (sym.type != SymbolType::Function || sym.isSynthesized)
            {
                continue;
            }
            const bool exactMatch =
                (!winner.isSynthesized && winner.fileUri == sym.fileUri && winner.selectionRange == sym.selectionRange);
            const bool mixinMatch = (winner.isSynthesized && target.compatibleClasses.contains(winner.containerName) &&
                                     HasSameParameterList(winner, sym, ctx.request.symbolTable.GetDefaultArrayType()));
            if (exactMatch || mixinMatch)
            {
                target.seenRefs.insert({std::string(evalCtx.fileUri), pos});
                break;
            }
        }
    }
}

void CountOperatorSiteReferences(std::string_view fileUri, const Scope* root, const BatchReferenceContext& ctx)
{
    if (!root)
    {
        return;
    }
    const IndexedOperatorSites* sites = root->GetOperatorSites();
    if (!sites || sites->sites.empty())
    {
        return;
    }
    const OperatorTargetInterest interest = ComputeTargetInterest(ctx);
    if (!interest.hasAnyOperator)
    {
        return;
    }

    OperatorEvalContext evalCtx{
        .fileUri = fileUri,
        .rootScope = root,
        .sites = *sites,
        .batch = ctx,
        .interest = interest,
        .exprMemo = std::vector<std::string>(sites->exprs.size()),
        .exprState = std::vector<uint8_t>(sites->exprs.size(), 0),
    };

    for (const auto& site : sites->sites)
    {
        const Scope* scope = FindInnermostScope(root, site.scopeLine, site.scopeCharacter);
        if (!scope)
        {
            scope = root;
        }
        ExprTypeEvalContext exprCtx{scope, *sites, ctx.request.symbolTable, evalCtx.exprMemo, evalCtx.exprState};
        switch (site.kind)
        {
        case OperatorSiteKind::Binary:
            if (interest.hasBinary || interest.hasImplConv)
            {
                EvaluateBinarySite(site, evalCtx, exprCtx);
            }
            break;
        case OperatorSiteKind::Assignment:
            if (interest.hasAssign || interest.hasImplConv || interest.hasImplCast)
            {
                EvaluateAssignmentSite(site, evalCtx, exprCtx);
            }
            if (interest.hasImplConv || interest.hasImplCast)
            {
                EvaluateCallForeachOrConversionSite(site, evalCtx);
            }
            break;
        case OperatorSiteKind::Unary:
            if (interest.hasUnary || interest.hasImplConv)
            {
                EvaluateUnarySite(site, evalCtx, exprCtx);
            }
            break;
        case OperatorSiteKind::Index:
            if (interest.hasIndex)
            {
                EvaluateIndexSite(site, evalCtx, exprCtx);
            }
            break;
        case OperatorSiteKind::Cast:
        case OperatorSiteKind::Call:
        case OperatorSiteKind::Foreach:
        case OperatorSiteKind::ImplicitConversion:
            EvaluateCallForeachOrConversionSite(site, evalCtx);
            break;
        }
    }
}
} // namespace angel_lsp::analysis
