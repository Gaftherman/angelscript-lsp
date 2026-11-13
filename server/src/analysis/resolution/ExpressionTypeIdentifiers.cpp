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

std::string ResolveScopedEnumMember(std::string_view whole, std::span<const Symbol> scopeSymbols,
                                    const SymbolTable& symbolTable)
{
    const auto [qualView, mem] = utils::SplitScopedName(whole);
    if (qualView.empty())
    {
        return "";
    }
    const auto qSymsPtr = symbolTable.FindSymbolsPtr(qualView);
    const bool hasQSyms = qSymsPtr && !qSymsPtr->empty();
    const auto checkEnums = [&](const auto& symList) -> std::string
    {
        for (const auto& qSym : symList)
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
    };
    if (hasQSyms)
    {
        if (auto res = checkEnums(*qSymsPtr); !res.empty())
        {
            return res;
        }
    }
    return checkEnums(scopeSymbols);
}

std::string ResolveScopedIdentifierType(std::string_view scopedText, std::span<const Symbol> wholeScopeSymbols,
                                        std::span<const Symbol> qualScopeSymbols, const SymbolTable& symbolTable)
{
    std::string_view lookupPath =
        utils::HasLeadingGlobalPrefix(scopedText) ? utils::StripGlobalPrefix(scopedText) : scopedText;
    if (const auto syms = symbolTable.FindSymbolsPtr(lookupPath))
    {
        if (std::string type = ExtractVariableOrPropertyType(*syms, symbolTable); !type.empty())
        {
            return type;
        }
    }
    if (std::string type = ExtractVariableOrPropertyType(wholeScopeSymbols, symbolTable); !type.empty())
    {
        return type;
    }
    return ResolveScopedEnumMember(lookupPath, qualScopeSymbols, symbolTable);
}

std::string ResolveIdentifierAccessorFallback(std::string_view name, const SymbolTable& symbolTable)
{
    for (const auto& prefix : {"get_", "set_"})
    {
        std::string fullName;
        fullName.reserve(std::string_view(prefix).size() + name.size());
        fullName.append(prefix);
        fullName.append(name);
        if (const auto accessorSyms = symbolTable.FindSymbolsPtr(fullName))
        {
            for (const auto& sym : *accessorSyms)
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
    }
    return "";
}

std::vector<Symbol> ResolveEnclosingContainerMemberSymbols(std::string_view name,
                                                           std::span<const ContainerInfo> containers,
                                                           const SymbolTable& symbolTable, ExpressionTypeCache* cache)
{
    for (const auto& c : containers)
    {
        if (c.kind != ContainerKind::Class && c.kind != ContainerKind::Interface)
        {
            continue;
        }
        const std::string_view typeName =
            c.qualifiedName.empty() ? std::string_view(c.name) : std::string_view(c.qualifiedName);
        auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable, cache);
        ExpandMixinHostHierarchies(typeName, symbolTable, hierarchy, cache);
        for (const auto& cls : hierarchy)
        {
            auto found = FilterSymbolsByContainerKind(symbolTable.FindMemberSymbols(cls, name), c.kind);
            if (!found.empty())
            {
                return found;
            }
            auto accessors = FindPropertyAccessors(cls, name, symbolTable, false);
            if (!accessors.empty())
            {
                return accessors;
            }
        }
        break;
    }
    return {};
}

std::string ResolveIdentifierHostFallbackFromHierarchy(std::string_view name, std::string_view hostClass,
                                                       const SymbolTable& symbolTable, ExpressionTypeCache* cache)
{
    if (hostClass.empty())
    {
        return "";
    }
    auto hierarchy = GetInheritedTypeHierarchy(hostClass, symbolTable, cache);
    for (const auto& cls : hierarchy)
    {
        if (const auto found = symbolTable.FindMemberSymbolPtr(cls, name))
        {
            for (const auto& sym : *found)
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
        auto accessors = FindPropertyAccessors(cls, name, symbolTable, false);
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

std::string ResolveThisType(std::string_view hostClass, std::span<const ContainerInfo> containers)
{
    if (!hostClass.empty())
    {
        return CleanExpressionType(hostClass);
    }
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            return CleanExpressionType(c.qualifiedName.empty() ? std::string_view(c.name)
                                                               : std::string_view(c.qualifiedName));
        }
    }
    return "";
}

std::string ExtractFirstSymbolTypeOrAccessor(std::span<const Symbol> syms, const SymbolTable& symbolTable)
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
