#include "analysis/resolution/ExpressionTypeUnary.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/Utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

bool IsFunctionHandleTargetName(std::string_view targetName, std::span<const Symbol> inScopeSymbols,
                                const SymbolTable& symbolTable)
{
    if (targetName.empty() || targetName.find('(') != std::string_view::npos)
    {
        return false;
    }
    if (const auto bucket = symbolTable.FindSymbolsPtr(targetName))
    {
        for (const auto& candidate : *bucket)
        {
            if (candidate.type == SymbolType::Function)
            {
                return true;
            }
        }
    }
    for (const auto& candidate : inScopeSymbols)
    {
        if (candidate.type == SymbolType::Function)
        {
            return true;
        }
    }
    return false;
}

std::string ResolveUnaryMinus(std::string_view operandType)
{
    const std::string_view clean = CleanExpressionTypeView(operandType);
    if (clean == parser::primitives::UInt || clean == "uint32")
    {
        return std::string(parser::primitives::Int);
    }
    if (clean == "uint8")
    {
        return "int8";
    }
    if (clean == "uint16")
    {
        return "int16";
    }
    if (clean == "uint64")
    {
        return "int64";
    }
    return std::string(operandType);
}

std::string ResolveUnaryHandle(bool isFunctionTarget, std::string_view operandType)
{
    if (isFunctionTarget)
    {
        return {};
    }
    if (operandType.empty())
    {
        return {};
    }
    const std::string_view clean = CleanExpressionTypeView(operandType);
    return IsHandleTypeView(clean) ? std::string(clean) : std::string(clean) + "@";
}

} // namespace angel_lsp::analysis
