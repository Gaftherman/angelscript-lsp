#include "analysis/resolution/ExpressionTypeIdentifiers.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

std::string ExtractVariableOrPropertyType(std::span<const Symbol> syms, const SymbolTable& table)
{
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, table);
            }
            return CleanExpressionType(type);
        }
    }
    return {};
}

std::string ResolveScopedEnumMember(std::string_view whole,
                                    const std::vector<Symbol>& scopeSymbols,
                                    const SymbolTable& symbolTable)
{
    const auto [qualView, mem] = utils::SplitScopedName(whole);
    if (qualView.empty())
    {
        return "";
    }
    std::string qual(qualView);
    auto qSyms = symbolTable.FindSymbols(qual);
    if (qSyms.empty())
    {
        qSyms = scopeSymbols;
    }
    for (const auto& qSym : qSyms)
    {
        if (qSym.type == SymbolType::Enum && std::holds_alternative<EnumSignature>(qSym.signature))
        {
            for (const auto& m : qSym.GetEnum().members)
            {
                if (m.name == mem)
                {
                    return qSym.qualifiedName.empty() ? qSym.name : qSym.qualifiedName;
                }
            }
        }
    }
    return "";
}

std::string ResolveIdentifierAccessorFallback(std::string_view name, const SymbolTable& symbolTable)
{
    std::string nameStr(name);
    for (const auto& prefix : {"get_", "set_"})
    {
        auto accessorSyms = symbolTable.FindSymbols(std::string(prefix) + nameStr);
        for (const auto& sym : accessorSyms)
        {
            if (sym.type == SymbolType::Function && sym.containerName.empty() &&
                std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                const bool isIndexed = (sym.name.starts_with("get_") && !fn.parameters.empty()) ||
                                       (sym.name.starts_with("set_") && fn.parameters.size() > 1);
                if (isIndexed)
                {
                    continue;
                }
                if (!fn.returnType.empty() && fn.returnType != "void")
                {
                    return CleanExpressionType(fn.returnType);
                }
                if (!fn.parameters.empty())
                {
                    return CleanExpressionType(fn.parameters.front().typeName);
                }
            }
        }
    }
    return "";
}

std::vector<Symbol> ResolveEnclosingContainerMemberSymbols(std::string_view name,
                                                          const std::vector<ContainerInfo>& containers,
                                                          const SymbolTable& symbolTable,
                                                          ExpressionTypeCache* cache)
{
    std::string nameStr(name);
    for (const auto& c : containers)
    {
        if (c.kind != ContainerKind::Class && c.kind != ContainerKind::Interface)
        {
            continue;
        }
        const std::string& typeName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
        auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable, cache);
        ExpandMixinHostHierarchies(typeName, symbolTable, hierarchy, cache);
        for (const auto& cls : hierarchy)
        {
            auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, nameStr), c.kind);
            if (!found.empty())
            {
                return found;
            }
            auto accessors = FindPropertyAccessors(cls, nameStr, symbolTable, false);
            if (!accessors.empty())
            {
                return accessors;
            }
        }
        break;
    }
    return {};
}

std::string ResolveIdentifierHostFallbackFromHierarchy(std::string_view name,
                                                       const std::string& hostClass,
                                                       const SymbolTable& symbolTable,
                                                       ExpressionTypeCache* cache)
{
    if (hostClass.empty())
    {
        return "";
    }
    std::string nameStr(name);
    auto hierarchy = GetInheritedTypeHierarchy(hostClass, symbolTable, cache);
    for (const auto& cls : hierarchy)
    {
        auto found = symbolTable.FindMemberSymbols(cls, nameStr);
        if (!found.empty())
        {
            for (const auto& sym : found)
            {
                if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
                    !sym.GetVariable().typeName.empty())
                {
                    std::string type = sym.GetVariable().typeName;
                    if (!sym.containerName.empty())
                    {
                        type = QualifyTypeInContainer(type, sym.containerName, symbolTable);
                    }
                    return CleanExpressionType(type);
                }
            }
            break;
        }
    }
    for (const auto& cls : hierarchy)
    {
        auto accessors = FindPropertyAccessors(cls, nameStr, symbolTable, false);
        if (!accessors.empty())
        {
            std::string pType = PropertyTypeFromAccessors(accessors);
            if (!pType.empty())
            {
                return CleanExpressionType(pType);
            }
        }
    }
    return "";
}

std::string ExtractFirstSymbolTypeOrAccessor(const std::vector<Symbol>& syms,
                                             const SymbolTable& symbolTable)
{
    for (const auto& sym : syms)
    {
        if ((sym.type == SymbolType::Variable || sym.type == SymbolType::Property) &&
            !sym.GetVariable().typeName.empty())
        {
            std::string type = sym.GetVariable().typeName;
            if (!sym.containerName.empty())
            {
                type = QualifyTypeInContainer(type, sym.containerName, symbolTable);
            }
            return CleanExpressionType(type);
        }
    }
    if (std::string pType = PropertyTypeFromAccessors(syms); !pType.empty())
    {
        return CleanExpressionType(pType);
    }
    return "";
}

} // namespace angel_lsp::analysis
