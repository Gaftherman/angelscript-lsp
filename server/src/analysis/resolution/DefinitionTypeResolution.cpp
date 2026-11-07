#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/DefinitionResolutionInternal.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

bool IsTypeDefType(SymbolType type) noexcept
{
    return type == SymbolType::Class || type == SymbolType::Interface || type == SymbolType::Enum ||
           type == SymbolType::Typedef || type == SymbolType::Funcdef;
}

std::string ResolveTargetTypeName(std::string_view nodeText, const DefinitionQuery& query)
{
    const std::string enclosingNs = FindEnclosingNamespace(query.uri, query.line, query.symbolTable);

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    if (rootScope)
    {
        const Scope* scope = FindInnermostScope(rootScope.get(), query.line, query.character);
        if (scope)
        {
            const LocalDefinition* def = ResolveInScopeAt(scope, nodeText, UINT32_MAX);
            if (def && !def->typeName.empty())
            {
                return CleanBaseType(def->typeName);
            }
        }
    }

    if (!enclosingNs.empty())
    {
        for (const auto& sym : query.symbolTable.FindMemberSymbols(enclosingNs, nodeText))
        {
            if (sym.type == SymbolType::Variable && !sym.GetVariable().typeName.empty())
            {
                return CleanBaseType(sym.GetVariable().typeName);
            }
            if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
            {
                return CleanBaseType(sym.GetFunction().returnType);
            }
        }
        for (const auto prefix : {"get_", "set_"})
        {
            const auto accSyms =
                query.symbolTable.FindMemberSymbols(enclosingNs, fmt::format("{}{}", prefix, nodeText));
            const std::string pType = PropertyTypeFromAccessors(accSyms);
            if (!pType.empty())
            {
                return CleanBaseType(pType);
            }
        }
    }

    auto symbols = query.symbolTable.FindSymbols(nodeText);
    for (const auto& sym : symbols)
    {
        if (sym.type == SymbolType::Variable && !sym.GetVariable().typeName.empty())
        {
            return CleanBaseType(sym.GetVariable().typeName);
        }
        if (sym.type == SymbolType::Function && !sym.GetFunction().returnType.empty())
        {
            return CleanBaseType(sym.GetFunction().returnType);
        }
        if (IsTypeDefType(sym.type))
        {
            return sym.name;
        }
    }

    auto accessors = FindGlobalPropertyAccessors(nodeText, query.symbolTable);
    std::string propType = PropertyTypeFromAccessors(accessors);
    if (!propType.empty())
    {
        return CleanBaseType(propType);
    }

    return CleanBaseType(nodeText);
}

std::vector<Symbol> ResolveTypeDefinitionTarget(const DefinitionQuery& query)
{
    if (query.sourceCode.empty() || query.identifier.empty() || !IsValidIdentifier(query.identifier))
    {
        return {};
    }

    std::string_view nodeText = query.identifier;

    std::string typeNameToFind = ResolveTargetTypeName(nodeText, query);
    if (typeNameToFind.empty() || IsPrimitiveTypeName(typeNameToFind))
    {
        return {};
    }

    const std::string enclosingNs = FindEnclosingNamespace(query.uri, query.line, query.symbolTable);
    std::vector<Symbol> typeSymbols;
    if (!enclosingNs.empty())
    {
        typeSymbols = query.symbolTable.FindMemberSymbols(enclosingNs, typeNameToFind);
    }
    if (typeSymbols.empty())
    {
        typeSymbols = query.symbolTable.FindSymbols(typeNameToFind);
    }
    if (typeSymbols.empty())
    {
        typeSymbols = query.symbolTable.FindTypeSymbolsByShortName(typeNameToFind);
    }

    if (typeSymbols.size() > 1 && !enclosingNs.empty())
    {
        std::vector<Symbol> filtered;
        for (const auto& sym : typeSymbols)
        {
            if (IsSameOrNestedNamespace(sym.containerName, enclosingNs))
            {
                filtered.push_back(sym);
            }
        }
        if (!filtered.empty())
        {
            typeSymbols = std::move(filtered);
        }
    }

    std::vector<Symbol> targetSymbols;
    for (const auto& sym : typeSymbols)
    {
        if (IsTypeDefType(sym.type))
        {
            targetSymbols.push_back(sym);
        }
    }
    return targetSymbols;
}

} // namespace angel_lsp::analysis
