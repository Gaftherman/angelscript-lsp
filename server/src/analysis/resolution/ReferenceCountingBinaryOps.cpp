#include "analysis/resolution/ReferenceCountingOperators.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace angel_lsp::analysis
{
static std::pair<std::string_view, std::string_view> BinaryOpMethods(std::string_view op) noexcept
{
    static constexpr std::array<std::pair<std::string_view, std::pair<std::string_view, std::string_view>>, 12> k_map =
        {{
            {"+", {"opAdd", "opAdd_r"}},
            {"-", {"opSub", "opSub_r"}},
            {"*", {"opMul", "opMul_r"}},
            {"/", {"opDiv", "opDiv_r"}},
            {"%", {"opMod", "opMod_r"}},
            {"**", {"opPow", "opPow_r"}},
            {"&", {"opAnd", "opAnd_r"}},
            {"|", {"opOr", "opOr_r"}},
            {"^", {"opXor", "opXor_r"}},
            {"<<", {"opShl", "opShl_r"}},
            {">>", {"opShr", "opShr_r"}},
            {">>>", {"opUShr", "opUShr_r"}},
        }};
    for (const auto& [tok, pair] : k_map)
    {
        if (tok == op)
        {
            return pair;
        }
    }
    return {};
}

static bool IsValidComparisonMethod(const Symbol& sym, std::string_view opName) noexcept
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& sig = sym.GetFunction();
    if (sig.parameters.size() != 1)
    {
        return false;
    }
    if (opName == "opEquals")
    {
        return sig.returnTypeKind == TypeKind::Bool && !sig.returnIsArray && !sig.modifiers.isHandle;
    }
    if (opName == "opCmp")
    {
        return sig.returnTypeKind == TypeKind::Int32 && !sig.returnIsArray && !sig.modifiers.isHandle;
    }
    return true;
}

static ResolvedOverloadMatch PickBestBidirectional(std::string_view leftType, std::string_view rightType,
                                                   std::pair<std::string_view, std::string_view> methods,
                                                   const SymbolTable& symbolTable)
{
    const ResolvedOverloadMatch fwd =
        ResolveSingleArgOverload({leftType, methods.first, {}, false}, rightType, symbolTable);
    const ResolvedOverloadMatch rev =
        ResolveSingleArgOverload({rightType, methods.second, {}, false}, leftType, symbolTable);
    if (fwd.hasMatch && rev.hasMatch)
    {
        return (rev.primaryConversion < fwd.primaryConversion) ? rev : fwd;
    }
    return fwd.hasMatch ? fwd : rev;
}

void EvaluateBinarySite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx)
{
    const std::string leftType = ResolveIndexedExprType(site.exprId, ctx);
    const std::string rightType = ResolveIndexedExprType(site.rhsExprId, ctx);
    if (leftType.empty() || rightType.empty())
    {
        return;
    }
    const SymbolTable& table = evalCtx.batch.request.symbolTable;
    const std::string_view op = site.op;

    if (op == "==" || op == "!=")
    {
        if (site.leftIsHandle || site.rightIsHandle || leftType == "null" || rightType == "null")
        {
            return;
        }
        const auto eq = PickBestBidirectional(leftType, rightType, {"opEquals", "opEquals"}, table);
        if (eq.hasMatch && IsValidComparisonMethod(eq.symbol, "opEquals"))
        {
            RecordMatchedOperatorSymbol(eq.symbol, site.line, site.character, evalCtx);
            return;
        }
        const auto cmp = PickBestBidirectional(leftType, rightType, {"opCmp", "opCmp"}, table);
        if (cmp.hasMatch && IsValidComparisonMethod(cmp.symbol, "opCmp"))
        {
            RecordMatchedOperatorSymbol(cmp.symbol, site.line, site.character, evalCtx);
        }
        return;
    }

    if (op == "<" || op == "<=" || op == ">" || op == ">=")
    {
        const auto cmp = PickBestBidirectional(leftType, rightType, {"opCmp", "opCmp"}, table);
        if (cmp.hasMatch && IsValidComparisonMethod(cmp.symbol, "opCmp"))
        {
            RecordMatchedOperatorSymbol(cmp.symbol, site.line, site.character, evalCtx);
        }
        return;
    }

    const auto [fwdName, revName] = BinaryOpMethods(op);
    if (fwdName.empty())
    {
        return;
    }
    const bool leftConst = IsIndexedExprConst(site.exprId, ctx);
    const bool rightConst = IsIndexedExprConst(site.rhsExprId, ctx);
    const ResolvedOverloadMatch fwd = ResolveSingleArgOverload({leftType, fwdName, {}, leftConst}, rightType, table);
    const ResolvedOverloadMatch rev = ResolveSingleArgOverload({rightType, revName, {}, rightConst}, leftType, table);
    if (fwd.hasMatch && rev.hasMatch)
    {
        const auto& chosen = (rev.primaryConversion < fwd.primaryConversion) ? rev : fwd;
        RecordMatchedOperatorSymbol(chosen.symbol, site.line, site.character, evalCtx);
    }
    else if (fwd.hasMatch)
    {
        RecordMatchedOperatorSymbol(fwd.symbol, site.line, site.character, evalCtx);
    }
    else if (rev.hasMatch)
    {
        RecordMatchedOperatorSymbol(rev.symbol, site.line, site.character, evalCtx);
    }
    else if (evalCtx.interest.hasImplConv)
    {
        const std::string_view cleanL = CleanBaseTypeView(leftType, table.GetDefaultArrayType());
        const std::string_view cleanR = CleanBaseTypeView(rightType, table.GetDefaultArrayType());
        if (IsPrimitiveTypeName(cleanR) && !IsPrimitiveTypeName(cleanL))
        {
            if (const auto conv = FindConversionOperator({leftType, rightType, "opImplConv", leftConst}, table))
            {
                RecordMatchedOperatorSymbol(*conv, site.line, site.character, evalCtx);
            }
        }
        else if (IsPrimitiveTypeName(cleanL) && !IsPrimitiveTypeName(cleanR))
        {
            if (const auto conv = FindConversionOperator({rightType, leftType, "opImplConv", rightConst}, table))
            {
                RecordMatchedOperatorSymbol(*conv, site.line, site.character, evalCtx);
            }
        }
    }
}
} // namespace angel_lsp::analysis
