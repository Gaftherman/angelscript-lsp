#include "analysis/resolution/ExpressionTypeCast.h"

#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
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

std::string ResolveLambdaTargetFuncdefType(const LambdaTargetFact& target, const SymbolTable& symbolTable)
{
    if (auto resolved = FuncdefTargetOfLambda(target, symbolTable))
    {
        return resolved->qualifiedName.empty() ? resolved->name : resolved->qualifiedName;
    }
    return "";
}

} // namespace angel_lsp::analysis
