#include "analysis/resolution/ExpressionTypeUnary.h"

#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/Utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

bool IsFunctionHandleTargetName(std::string_view targetName,
                                const std::vector<Symbol>& inScopeSymbols,
                                const SymbolTable& symbolTable)
{
    if (targetName.empty() || targetName.find('(') != std::string_view::npos)
    {
        return false;
    }
    std::string nameStr(targetName);
    if (const auto bucket = symbolTable.FindSymbolsPtr(nameStr))
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
    auto syms = symbolTable.FindSymbols(nameStr);
    for (const auto& candidate : syms)
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
    std::string clean = CleanExpressionType(operandType);
    if (clean == "uint" || clean == "uint32")
    {
        return "int";
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
    if (ExpressionTypeCache::IsPoisonType(operandType))
    {
        return std::string(ExpressionTypeCache::k_poisonType);
    }
    if (operandType.empty())
    {
        return {};
    }
    std::string clean = CleanExpressionType(operandType);
    return IsHandleTypeView(clean) ? clean : clean + "@";
}

} // namespace angel_lsp::analysis
