#include "analysis/resolution/ExpressionTypeBinary.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

static std::pair<std::string_view, std::string_view> GetBinaryOpMethods(std::string_view op)
{
    if (op == "+")
        return {"opAdd", "opAdd_r"};
    if (op == "-")
        return {"opSub", "opSub_r"};
    if (op == "*")
        return {"opMul", "opMul_r"};
    if (op == "/")
        return {"opDiv", "opDiv_r"};
    if (op == "%")
        return {"opMod", "opMod_r"};
    if (op == "**")
        return {"opPow", "opPow_r"};
    if (op == "&")
        return {"opAnd", "opAnd_r"};
    if (op == "|")
        return {"opOr", "opOr_r"};
    if (op == "^")
        return {"opXor", "opXor_r"};
    if (op == "<<")
        return {"opShl", "opShl_r"};
    if (op == ">>")
        return {"opShr", "opShr_r"};
    if (op == ">>>")
        return {"opUShr", "opUShr_r"};
    return {"", ""};
}

static BinaryOverloadResult ResolveBinaryOverloadMatch(const std::string& typeName, std::string_view opMethod,
                                                       const std::string& argType, const SymbolTable& symbolTable)
{
    if (opMethod.empty() || typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return {};
    }
    std::vector<Symbol> candidates;
    auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable);
    for (const auto& t : hierarchy)
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(t, opMethod))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
    if (!candidates.empty())
    {
        auto match = ResolveBestOverload(candidates, {argType}, symbolTable);
        const Symbol* best = match.bestCandidate ? match.bestCandidate : match.priorityCandidate;
        if (best && std::holds_alternative<FunctionSignature>(best->signature))
        {
            return {true, std::move(match.bestConversions), CleanExpressionType(best->GetFunction().returnType)};
        }
    }
    return {};
}

static std::string FindSingleOpImplConvPrimitive(const std::string& typeName, const SymbolTable& symbolTable)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return "";
    }
    std::string matchedPrim;
    for (const auto& t : GetInheritedTypeHierarchy(typeName, symbolTable))
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(t, "opImplConv"))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
                {
                    std::string ret = CleanBaseType(sym.GetFunction().returnType);
                    if (parser::primitives::IsNumeric(ret) || ret == "bool")
                    {
                        if (!matchedPrim.empty() && matchedPrim != ret)
                        {
                            return "";
                        }
                        matchedPrim = std::move(ret);
                    }
                }
            }
        }
    }
    return matchedPrim;
}

static std::string SelectBestBinaryOverload(const BinaryOverloadResult& forwardMatch,
                                            const BinaryOverloadResult& reverseMatch)
{
    if (forwardMatch.hasMatch && reverseMatch.hasMatch)
    {
        if (!forwardMatch.bestConversions.empty() && !reverseMatch.bestConversions.empty())
        {
            if (reverseMatch.bestConversions[0] < forwardMatch.bestConversions[0])
            {
                return reverseMatch.returnType;
            }
            return forwardMatch.returnType;
        }
        return forwardMatch.returnType;
    }
    if (forwardMatch.hasMatch)
    {
        return forwardMatch.returnType;
    }
    if (reverseMatch.hasMatch)
    {
        return reverseMatch.returnType;
    }
    return {};
}

static std::string ResolveBinaryFallbackConv(std::string_view op, const std::string& cleanLeft,
                                             const std::string& cleanRight, const ExpressionTypeContext& ctx)
{
    const bool leftIsPrim = parser::primitives::IsPrimitive(cleanLeft);
    const bool rightIsPrim = parser::primitives::IsPrimitive(cleanRight);
    if (!leftIsPrim && rightIsPrim)
    {
        if (auto conv = FindSingleOpImplConvPrimitive(cleanLeft, ctx.symbolTable); !conv.empty())
        {
            return ResolveBinaryPrimitivePromotion(op, conv, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
        }
    }
    else if (leftIsPrim && !rightIsPrim)
    {
        if (auto conv = FindSingleOpImplConvPrimitive(cleanRight, ctx.symbolTable); !conv.empty())
        {
            return ResolveBinaryPrimitivePromotion(op, cleanLeft, conv, ctx.disableIntegerDivision, &ctx.symbolTable);
        }
    }
    return {};
}

static std::string ResolveStringConcatenation(std::string_view op, const std::string& leftType,
                                              const std::string& rightType, const ExpressionTypeContext& ctx)
{
    if (op != "+")
    {
        return {};
    }
    const std::string_view effectiveStr =
        ctx.stringTypeName.empty() ? ctx.symbolTable.GetStringType() : ctx.stringTypeName;
    if (CleanBaseType(leftType, ctx.arrayTypeName) == effectiveStr ||
        CleanBaseType(rightType, ctx.arrayTypeName) == effectiveStr)
    {
        return std::string(effectiveStr);
    }
    return {};
}

std::string ResolveBinaryOverload(std::string_view op, const std::string& leftType, const std::string& rightType,
                                  const SymbolTable& symbolTable)
{
    const std::string cleanLeft = CleanBaseType(leftType);
    const std::string cleanRight = CleanBaseType(rightType);
    const bool leftIsPrim = parser::primitives::IsPrimitive(cleanLeft);
    const bool rightIsPrim = parser::primitives::IsPrimitive(cleanRight);

    auto [opMethod, revOpMethod] = GetBinaryOpMethods(op);

    BinaryOverloadResult forwardMatch;
    if (!leftIsPrim)
    {
        forwardMatch = ResolveBinaryOverloadMatch(cleanLeft, opMethod, rightType, symbolTable);
    }

    BinaryOverloadResult reverseMatch;
    if (!rightIsPrim)
    {
        reverseMatch = ResolveBinaryOverloadMatch(cleanRight, revOpMethod, leftType, symbolTable);
    }

    return SelectBestBinaryOverload(forwardMatch, reverseMatch);
}

std::string EvaluateBinaryResultType(std::string_view op, const std::string& leftType,
                                     const std::string& rightType, const ExpressionTypeContext& ctx)
{
    if (IsBooleanBinaryOp(op))
    {
        return "bool";
    }
    if (std::string strType = ResolveStringConcatenation(op, leftType, rightType, ctx); !strType.empty())
    {
        return strType;
    }

    std::string cleanLeft = CleanBaseType(leftType);
    std::string cleanRight = CleanBaseType(rightType);

    if (parser::primitives::IsPrimitive(cleanLeft) && parser::primitives::IsPrimitive(cleanRight))
    {
        return ResolveBinaryPrimitivePromotion(op, cleanLeft, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
    }

    if (std::string overloadType = ResolveBinaryOverload(op, leftType, rightType, ctx.symbolTable);
        !overloadType.empty())
    {
        return overloadType;
    }

    if (std::string fallbackType = ResolveBinaryFallbackConv(op, cleanLeft, cleanRight, ctx); !fallbackType.empty())
    {
        return fallbackType;
    }

    return ResolveBinaryPrimitivePromotion(op, cleanLeft, cleanRight, ctx.disableIntegerDivision, &ctx.symbolTable);
}

} // namespace angel_lsp::analysis
