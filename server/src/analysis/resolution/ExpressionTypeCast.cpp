#include "analysis/resolution/ExpressionTypeCast.h"

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

std::string ResolveConstructCallType(std::string_view typeText, std::string_view templateText)
{
    if (typeText.empty())
    {
        return "";
    }
    std::string written(typeText);
    if (!templateText.empty())
    {
        written += std::string(templateText);
    }
    return CleanExpressionType(written);
}

std::string ResolveCastExpressionType(std::string_view typeText)
{
    if (typeText.empty())
    {
        return "";
    }
    return CleanExpressionType(typeText);
}

std::string ResolveLambdaTargetFuncdefType(std::string_view targetName, const SymbolTable& symbolTable)
{
    if (auto target = FuncdefTargetOfLambda(targetName, symbolTable))
    {
        return target->qualifiedName.empty() ? target->name : target->qualifiedName;
    }
    return "";
}

} // namespace angel_lsp::analysis
