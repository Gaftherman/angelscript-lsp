#include "analysis/resolution/PropertyAndDelegateResolver.h"

#include "analysis/index/RuleIndex.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/AccessorNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
std::string PropertyNameFromAccessor(const Symbol& sym, bool keywordRequired)
{
    if (!std::holds_alternative<FunctionSignature>(sym.signature))
        return {};

    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
        return {};

    return std::string(utils::AccessorPropertyName(sym.name));
}

std::vector<Symbol> FindPropertyAccessors(std::string_view typeName, std::string_view propertyName,
                                          const SymbolTable& symbolTable, bool keywordRequired)
{
    std::vector<Symbol> accessors;
    if (typeName.empty() || propertyName.empty())
        return accessors;

    auto hierarchy = GetInheritedTypeHierarchy(typeName, symbolTable);
    ExpandMixinHostHierarchies(typeName, symbolTable, hierarchy);
    for (const std::string_view prefix : {std::string_view("get_"), std::string_view("set_")})
    {
        std::string accessorName(prefix);
        accessorName += propertyName;
        for (const auto& owner : hierarchy)
        {
            bool foundForOwner = false;
            if (const auto syms = symbolTable.FindMemberSymbolPtr(owner, accessorName))
            {
                for (const auto& sym : *syms)
                {
                    if (!std::holds_alternative<FunctionSignature>(sym.signature))
                        continue;
                    if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
                        continue;

                    accessors.push_back(sym);
                    foundForOwner = true;
                }
            }

            // A derived class overriding this accessor answers for it; the base's copy would only
            // be the same property said twice.
            if (foundForOwner)
                break;
        }
    }

    return accessors;
}

std::vector<Symbol> FindGlobalPropertyAccessors(std::string_view propertyName, const SymbolTable& symbolTable,
                                                bool keywordRequired)
{
    std::vector<Symbol> accessors;
    if (propertyName.empty())
    {
        return accessors;
    }

    char buf[128];
    for (const std::string_view prefix : {std::string_view("get_"), std::string_view("set_")})
    {
        if (prefix.size() + propertyName.size() >= sizeof(buf))
        {
            continue;
        }
        memcpy(buf, prefix.data(), prefix.size());
        memcpy(buf + prefix.size(), propertyName.data(), propertyName.size());
        const std::string_view key(buf, prefix.size() + propertyName.size());

        const auto syms = symbolTable.FindSymbolsPtr(key);
        if (!syms)
        {
            continue;
        }

        for (const auto& sym : *syms)
        {
            if (sym.type != SymbolType::Function || !sym.containerName.empty())
            {
                continue;
            }
            if (!std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            if (keywordRequired && !sym.GetFunction().modifiers.isProperty)
            {
                continue;
            }

            accessors.push_back(sym);
        }
    }

    return accessors;
}

std::string PropertyTypeFromAccessors(const std::vector<Symbol>& accessors)
{
    for (const auto& sym : accessors)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            continue;

        const FunctionSignature& func = sym.GetFunction();
        if (sym.name.compare(0, 4, "get_") == 0)
            return func.returnType;
    }

    for (const auto& sym : accessors)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
            continue;

        const FunctionSignature& func = sym.GetFunction();
        if (sym.name.compare(0, 4, "set_") == 0 && !func.parameters.empty())
            return func.parameters.front().typeName;
    }

    return {};
}

bool IsBaseClassDelegateProperty(std::string_view name, const SymbolTable& symbolTable,
                                 const config::EngineRuleConfig* engineRules)
{
    std::span<const std::string> delegateProps;
    if (engineRules && !engineRules->unusedRules.baseClassDelegateProperties.empty())
    {
        delegateProps = engineRules->unusedRules.baseClassDelegateProperties;
    }
    else
    {
        delegateProps = symbolTable.GetBaseClassDelegateProperties();
    }
    if (delegateProps.empty())
    {
        if (const auto ruleIndex = symbolTable.GetRuleIndex())
        {
            delegateProps = ruleIndex->baseClassDelegateProperties;
        }
    }
    return std::any_of(delegateProps.begin(), delegateProps.end(), [&](const std::string& p) { return p == name; });
}

static std::string FindEnclosingClassOrHost(TSNode node, std::string_view sourceCode, const SymbolTable& symbolTable,
                                            std::string_view virtualHostClass, const NodeIndex* nodeIndex)
{
    if (!virtualHostClass.empty())
    {
        return std::string(virtualHostClass);
    }
    std::string hostClass =
        nodeIndex ? InferHostClassFromEnclosingMixin(IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)},
                                                     *nodeIndex, sourceCode, symbolTable)
                  : "";
    if (!hostClass.empty())
    {
        return hostClass;
    }
    for (const auto& c : GetEnclosingContainers(node, sourceCode, nodeIndex))
    {
        if (c.kind == ContainerKind::Class)
        {
            return c.qualifiedName.empty() ? c.name : c.qualifiedName;
        }
    }
    return "";
}

std::string ResolveDelegateTargetBaseClass(const DelegateTargetClassRequest& req)
{
    std::string targetClass =
        FindEnclosingClassOrHost(req.node, req.sourceCode, req.symbolTable, req.virtualHostClass, req.nodeIndex);
    if (targetClass.empty())
    {
        return "";
    }
    std::string baseClass = ResolveBaseClass(targetClass, req.symbolTable);
    if (baseClass.empty() && req.engineRules)
    {
        auto it = req.engineRules->unusedRules.baseClassProxyMap.find(targetClass);
        if (it != req.engineRules->unusedRules.baseClassProxyMap.end() && !it->second.empty())
        {
            baseClass = CleanBaseType(it->second.front());
        }
    }
    return baseClass;
}

std::string ResolveDelegatePropertyReceiver(const DelegatePropertyReceiverContext& dctx, const SymbolTable& symbolTable,
                                            const ReceiverTypeContext& ctx)
{
    if (!IsBaseClassDelegateProperty(dctx.objText, symbolTable, ctx.engineRules))
    {
        return "";
    }
    std::string baseClass = ResolveDelegateTargetBaseClass(
        {dctx.objNode, dctx.sourceCode, symbolTable, dctx.effectiveHostClass, ctx.engineRules, dctx.nodeIndex});
    if (!baseClass.empty())
    {
        return MemberOwnerType(baseClass, dctx.arrayTypeName);
    }
    return "";
}

} // namespace angel_lsp::analysis
