#include "analysis/resolution/ExpressionTypeIndex.h"

#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

std::string FindIndexedGetterType(const std::vector<Symbol>& accessors, const SymbolTable* symbolTable,
                                  std::string_view indexArgType)
{
    std::vector<Symbol> getters;
    for (const auto& sym : accessors)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            const auto& fn = sym.GetFunction();
            if (sym.name.starts_with("get_") && !fn.parameters.empty())
            {
                getters.push_back(sym);
            }
        }
    }
    if (getters.empty())
    {
        return "";
    }
    if (symbolTable && !indexArgType.empty())
    {
        auto match = ResolveBestOverload(getters, {std::string(indexArgType)}, *symbolTable);
        if (match.bestCandidate)
        {
            return CleanExpressionType(match.bestCandidate->GetFunction().returnType);
        }
    }
    return CleanExpressionType(getters.front().GetFunction().returnType);
}

std::string ResolveMemberIndexedPropertyType(std::string_view recType, std::string_view memName,
                                             const SymbolTable& symbolTable, std::string_view indexArgType)
{
    if (recType.empty() || memName.empty())
    {
        return "";
    }
    return FindIndexedGetterType(FindPropertyAccessors(std::string(recType), std::string(memName), symbolTable, false),
                                 &symbolTable, indexArgType);
}

std::string ResolveScopedIndexedPropertyType(std::string_view scopeName, std::string_view name,
                                             const SymbolTable& symbolTable, std::string_view indexArgType)
{
    if (scopeName.empty() || name.empty())
    {
        return "";
    }
    return FindIndexedGetterType(
        FindPropertyAccessors(std::string(scopeName), std::string(name), symbolTable, false),
        &symbolTable, indexArgType);
}

std::string ResolveBareIndexedPropertyType(std::string_view name, std::string_view enclosingClass,
                                           const SymbolTable& symbolTable, std::string_view indexArgType)
{
    if (name.empty())
    {
        return "";
    }
    if (std::string hit = FindIndexedGetterType(FindGlobalPropertyAccessors(std::string(name), symbolTable, false),
                                                &symbolTable, indexArgType);
        !hit.empty())
    {
        return hit;
    }
    if (!enclosingClass.empty())
    {
        if (std::string hit =
                FindIndexedGetterType(FindPropertyAccessors(std::string(enclosingClass), std::string(name), symbolTable, false),
                                      &symbolTable, indexArgType);
            !hit.empty())
        {
            return hit;
        }
    }
    return "";
}

} // namespace angel_lsp::analysis
