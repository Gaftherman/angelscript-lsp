#include "analysis/resolution/ClassHierarchyResolver.h"

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::analysis
{
bool IsMixinClass(std::string_view baseTypeName, const SymbolTable& table)
{
    if (baseTypeName.empty())
    {
        return false;
    }

    std::string searchName(baseTypeName);
    auto symsPtr = table.FindSymbolsPtr(searchName);
    if (!symsPtr)
    {
        return false;
    }

    for (const auto& sym : *symsPtr)
    {
        if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
        {
            return true;
        }
    }
    return false;
}

NonInstantiableKind ClassifyNonInstantiable(std::string_view baseTypeName, const SymbolTable& table)
{
    if (baseTypeName.empty())
    {
        return NonInstantiableKind::None;
    }

    auto symsPtr = table.FindSymbolsPtr(baseTypeName);
    if (!symsPtr)
    {
        return NonInstantiableKind::None;
    }

    for (const auto& sym : *symsPtr)
    {
        if (sym.type == SymbolType::Interface)
        {
            return NonInstantiableKind::Interface;
        }
        if (sym.type == SymbolType::Class)
        {
            if (sym.GetClass().modifiers.isMixin)
            {
                return NonInstantiableKind::Mixin;
            }
            if (sym.GetClass().modifiers.isAbstract)
            {
                return NonInstantiableKind::Abstract;
            }
        }
    }
    return NonInstantiableKind::None;
}

std::vector<Symbol> FindHierarchyTypeSymbols(const std::string& curType, const SymbolTable& symbolTable)
{
    const auto symbols = symbolTable.FindSymbolsPtr(curType);
    if (symbols && !symbols->empty())
    {
        const bool hasType = std::any_of(symbols->begin(), symbols->end(), [](const Symbol& s)
                                         { return s.type == SymbolType::Class || s.type == SymbolType::Interface; });
        if (hasType)
        {
            return *symbols;
        }
    }
    if (!utils::ContainsScopeSeparator(curType))
    {
        return symbolTable.FindTypeSymbolsByShortName(curType);
    }
    return {};
}

void EnqueueClassBases(const Symbol& sym, const SymbolTable& symbolTable,
                       ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& queue)
{
    const auto& cls = sym.GetClass();
    std::vector<HierarchyClassifiedBase> bases;
    bases.reserve(cls.bases.size());

    for (const auto& base : cls.bases)
    {
        std::string cleanBase = CleanBaseType(base);
        if (!cleanBase.empty())
        {
            const bool isMixin = IsMixinClass(cleanBase, symbolTable);
            bases.push_back(HierarchyClassifiedBase{std::move(cleanBase), isMixin});
        }
    }

    for (const bool wantMixins : {true, false})
    {
        for (const auto& base : bases)
        {
            if (base.isMixin == wantMixins && visited.insert(base.name).second)
            {
                queue.push_back(base.name);
            }
        }
    }
}

void EnqueueInterfaceBases(const Symbol& sym, ankerl::unordered_dense::set<std::string>& visited,
                           std::vector<std::string>& queue)
{
    const auto& iface = sym.GetInterface();
    for (const auto& base : iface.inheritedInterfaces)
    {
        std::string cleanBase = CleanBaseType(base);
        if (!cleanBase.empty() && visited.insert(cleanBase).second)
        {
            queue.push_back(cleanBase);
        }
    }
}

std::vector<std::string> GetInheritedTypeHierarchy(std::string_view className, const SymbolTable& symbolTable,
                                                   ExpressionTypeCache* cache)
{
    std::vector<std::string> hierarchy;
    const std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return hierarchy;
    }

    if (parser::primitives::IsPrimitive(rootType))
    {
        return {rootType};
    }

    if (cache)
    {
        auto it = cache->hierarchies.find(rootType);
        if (it != cache->hierarchies.end())
        {
            return it->second;
        }
    }

    ankerl::unordered_dense::set<std::string> visited;
    std::vector<std::string> queue;

    visited.insert(rootType);
    queue.push_back(rootType);

    size_t head = 0;
    while (head < queue.size())
    {
        const std::string curType = queue[head++];
        hierarchy.push_back(curType);

        const auto symbolsToIterate = FindHierarchyTypeSymbols(curType, symbolTable);
        for (const auto& sym : symbolsToIterate)
        {
            if (sym.type == SymbolType::Class)
            {
                const std::string qName = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
                if (visited.insert(qName).second)
                {
                    hierarchy.push_back(qName);
                }
                EnqueueClassBases(sym, symbolTable, visited, queue);
            }
            else if (sym.type == SymbolType::Interface)
            {
                EnqueueInterfaceBases(sym, visited, queue);
            }
        }
    }

    if (cache)
    {
        cache->hierarchies.emplace(rootType, hierarchy);
    }

    return hierarchy;
}

std::string ResolveBaseClass(std::string_view className, const SymbolTable& symbolTable)
{
    if (className.empty())
    {
        return "";
    }
    auto hier = GetInheritedTypeHierarchy(std::string(className), symbolTable);
    for (size_t i = 1; i < hier.size(); ++i)
    {
        if (!IsMixinClass(hier[i], symbolTable))
        {
            return hier[i];
        }
    }
    if (hier.size() > 1)
    {
        return hier[1];
    }
    return "";
}

bool IsDerivedFrom(std::string_view derivedClass, std::string_view baseClass, const SymbolTable& symbolTable)
{
    if (derivedClass == baseClass || IsSameType(std::string(derivedClass), std::string(baseClass)))
    {
        return true;
    }
    for (const auto& ancestor : GetInheritedTypeHierarchy(std::string(derivedClass), symbolTable))
    {
        if (ancestor == baseClass || IsSameType(ancestor, std::string(baseClass)))
        {
            return true;
        }
    }
    return false;
}

void ExtractTypesFromMemberSymbols(const std::vector<Symbol>& syms, std::string_view className,
                                   std::vector<std::string>& result)
{
    for (const auto& s : syms)
    {
        std::string rawType;
        if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            if (std::holds_alternative<VariableSignature>(s.signature))
            {
                const auto& var = s.GetVariable();
                rawType = !var.baseTypeName.empty() ? var.baseTypeName : var.typeName;
            }
        }
        else if (s.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(s.signature))
        {
            const auto& fn = s.GetFunction();
            rawType = !fn.returnBaseTypeName.empty() ? fn.returnBaseTypeName : fn.returnType;
        }

        std::string clean = CleanBaseType(rawType);
        if (!clean.empty() && clean != className && !IsPrimitiveTypeName(clean))
        {
            if (std::find(result.begin(), result.end(), clean) == result.end())
            {
                result.push_back(std::move(clean));
            }
        }
    }
}

void InspectDelegateProperty(std::string_view className, const std::string& prop, const SymbolTable& symbolTable,
                             std::vector<std::string>& result)
{
    if (const auto syms = symbolTable.FindMemberSymbolPtr(className, prop))
    {
        ExtractTypesFromMemberSymbols(*syms, className, result);
    }
    if (!prop.starts_with("get_"))
    {
        if (const auto getterSyms = symbolTable.FindMemberSymbolPtr(className, "get_" + prop))
        {
            ExtractTypesFromMemberSymbols(*getterSyms, className, result);
        }
    }
}

static bool TryApplyCachedMixinHierarchy(std::string_view containerClass, std::vector<std::string>& hierarchy,
                                         ExpressionTypeCache* cache)
{
    if (!cache)
    {
        return false;
    }
    auto cachedIt = cache->expandedMixinHierarchies.find(std::string(containerClass));
    if (cachedIt == cache->expandedMixinHierarchies.end())
    {
        return false;
    }
    for (const auto& hCls : cachedIt->second)
    {
        if (std::find(hierarchy.begin(), hierarchy.end(), hCls) == hierarchy.end())
        {
            hierarchy.push_back(hCls);
        }
    }
    return true;
}

void ExpandMixinHostHierarchies(std::string_view containerClass, const SymbolTable& symbolTable,
                                std::vector<std::string>& hierarchy, ExpressionTypeCache* cache)
{
    if (TryApplyCachedMixinHierarchy(containerClass, hierarchy, cache))
    {
        return;
    }

    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    auto mixinIt = ruleIndex->hostClassesByMixin.find(containerClass);
    if (mixinIt == ruleIndex->hostClassesByMixin.end() && utils::ContainsScopeSeparator(containerClass))
    {
        mixinIt = ruleIndex->hostClassesByMixin.find(LastScopeSegment(containerClass));
    }
    if (mixinIt == ruleIndex->hostClassesByMixin.end())
    {
        if (cache)
        {
            cache->expandedMixinHierarchies.emplace(std::string(containerClass), std::vector<std::string>{});
        }
        return;
    }

    std::vector<std::string> collected;
    for (const auto& host : mixinIt->second)
    {
        std::string hostKey = host.qualifiedName.empty() ? host.name : host.qualifiedName;
        auto hostHier = GetInheritedTypeHierarchy(hostKey, symbolTable, cache);
        for (auto& hCls : hostHier)
        {
            if (std::find(collected.begin(), collected.end(), hCls) == collected.end())
            {
                collected.push_back(hCls);
            }
        }
    }
    for (const auto& hCls : collected)
    {
        if (std::find(hierarchy.begin(), hierarchy.end(), hCls) == hierarchy.end())
        {
            hierarchy.push_back(hCls);
        }
    }
    if (cache)
    {
        cache->expandedMixinHierarchies.emplace(std::string(containerClass), std::move(collected));
    }
}

std::vector<std::string> ResolveLinkedBaseClasses(std::string_view className, const SymbolTable& symbolTable,
                                                  const LinkedBaseOptions& options)
{
    std::vector<std::string> result;
    if (className.empty() || !options.HasActiveOptions())
    {
        return result;
    }

    if (options.proxyMap)
    {
        auto it = options.proxyMap->find(className);
        if (it == options.proxyMap->end() && utils::ContainsScopeSeparator(className))
        {
            it = options.proxyMap->find(LastScopeSegment(className));
        }
        if (it != options.proxyMap->end())
        {
            for (const auto& target : it->second)
            {
                std::string clean = CleanBaseType(target);
                if (!clean.empty() && clean != className && !IsPrimitiveTypeName(clean) &&
                    std::find(result.begin(), result.end(), clean) == result.end())
                {
                    result.push_back(std::move(clean));
                }
            }
        }
    }

    for (const auto& prop : options.delegateProps)
    {
        InspectDelegateProperty(className, prop, symbolTable, result);
    }

    return result;
}

std::vector<Symbol> FindOverriddenBaseMethodSymbols(std::string_view containerClass, std::string_view methodName,
                                                    const SymbolTable& symbolTable, const LinkedBaseOptions& options)
{
    std::vector<Symbol> result;
    if (containerClass.empty() || methodName.empty())
    {
        return result;
    }

    std::vector<std::string> hierarchy = GetInheritedTypeHierarchy(std::string(containerClass), symbolTable);
    ExpandMixinHostHierarchies(containerClass, symbolTable, hierarchy);

    auto appendMatchingMethods = [&](std::string_view ownerClass)
    {
        const auto syms = symbolTable.FindMemberSymbolPtr(ownerClass, methodName);
        if (!syms)
        {
            return;
        }
        for (const auto& s : *syms)
        {
            if (s.type == SymbolType::Function)
            {
                result.push_back(s);
            }
        }
    };

    const bool hasLinkedOptions = options.HasActiveOptions();
    ankerl::unordered_dense::set<std::string> visitedOwners;
    for (const auto& ancestor : hierarchy)
    {
        if (ancestor != containerClass && visitedOwners.insert(ancestor).second)
        {
            appendMatchingMethods(ancestor);
        }

        if (hasLinkedOptions)
        {
            auto linkedBases = ResolveLinkedBaseClasses(ancestor, symbolTable, options);
            for (const auto& linkedBase : linkedBases)
            {
                if (linkedBase != containerClass && visitedOwners.insert(linkedBase).second)
                {
                    appendMatchingMethods(linkedBase);
                }
            }
        }
    }

    return result;
}

bool HierarchyIsFullyVisible(const std::string& typeName, const SymbolTable& symbolTable)
{
    for (const auto& ancestor : GetInheritedTypeHierarchy(typeName, symbolTable))
    {
        const auto symbols = symbolTable.FindSymbolsPtr(ancestor);
        if (!symbols)
        {
            return false;
        }

        // The hierarchy walk itself stops at a name it cannot resolve, so an unresolvable base
        // never appears in the list above and has to be looked for here, one level down.
        for (const auto& sym : *symbols)
        {
            if (sym.type != SymbolType::Class)
            {
                continue;
            }
            for (const auto& base : sym.GetClass().bases)
            {
                if (!symbolTable.FindSymbolsPtr(CleanBaseType(base)))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

void TraverseAncestorsAndInterfaces(const std::string& rootType, const SymbolTable& symbolTable,
                                    ankerl::unordered_dense::set<std::string>& visited,
                                    std::vector<std::string>& related)
{
    std::vector<std::string> queue;
    visited.insert(rootType);
    queue.push_back(rootType);

    size_t head = 0;
    while (head < queue.size())
    {
        const std::string curType = queue[head++];
        related.push_back(curType);

        const auto symbols = symbolTable.FindSymbolsPtr(curType);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type == SymbolType::Class)
            {
                for (const auto& base : sym.GetClass().bases)
                {
                    std::string clean = CleanBaseType(base);
                    if (!clean.empty() && visited.insert(clean).second)
                    {
                        queue.push_back(clean);
                    }
                }
            }
            else if (sym.type == SymbolType::Interface)
            {
                for (const auto& base : sym.GetInterface().inheritedInterfaces)
                {
                    std::string clean = CleanBaseType(base);
                    if (!clean.empty() && visited.insert(clean).second)
                    {
                        queue.push_back(clean);
                    }
                }
            }
        }
    }
}

/**
 * @brief Traverses derived classes iteratively across the inheritance rule index starting from a frontier.
 * @param[in] initialFrontier Initial list of class names to expand downwards in the inheritance tree.
 * @param[in] symbolTable Symbol table containing inheritance relationships and rule index.
 * @param[in,out] visited Set of already visited class names to prevent cycles and duplicate traversal.
 * @param[in,out] related Destination vector accumulating all discovered derived class names.
 */
void TraverseDerivedClasses(const std::vector<std::string>& initialFrontier, const SymbolTable& symbolTable,
                            ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& related)
{
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }

    std::vector<std::string> frontier = initialFrontier;
    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& baseName : frontier)
        {
            const auto it = ruleIndex->derivedByBase.find(baseName);
            if (it == ruleIndex->derivedByBase.end())
            {
                continue;
            }
            for (const auto& derived : it->second)
            {
                if (visited.insert(derived.qualifiedName).second)
                {
                    related.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);
                }
                if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                    visited.insert(derived.name).second)
                {
                    related.push_back(derived.name);
                    next.push_back(derived.name);
                }
            }
        }
        frontier = std::move(next);
    }
}

std::vector<std::string> GetAllRelatedClasses(const std::string& className, const SymbolTable& symbolTable)
{
    std::vector<std::string> related;
    const std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return related;
    }

    ankerl::unordered_dense::set<std::string> visited;
    TraverseAncestorsAndInterfaces(rootType, symbolTable, visited, related);

    const std::vector<std::string> frontier(related.begin(), related.end());
    TraverseDerivedClasses(frontier, symbolTable, visited, related);

    return related;
}

/**
 * @brief Traverses the inheritance graph downwards to collect all direct and indirect derived classes.
 * @param[in] className Root class name from which derived classes are queried.
 * @param[in] symbolTable Symbol table containing class definitions and inheritance relations.
 * @return Vector of class names inheriting from className.
 */
std::vector<std::string> GetDerivedClasses(const std::string& className, const SymbolTable& symbolTable)
{
    std::vector<std::string> derivedClasses;
    std::string rootType = CleanBaseType(className);
    if (rootType.empty())
    {
        return derivedClasses;
    }

    ankerl::unordered_dense::set<std::string> visited;
    visited.insert(rootType);

    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return derivedClasses;
    }

    std::vector<std::string> frontier;
    frontier.push_back(rootType);

    while (!frontier.empty())
    {
        std::vector<std::string> next;
        for (const auto& baseName : frontier)
        {
            const auto it = ruleIndex->derivedByBase.find(baseName);
            if (it == ruleIndex->derivedByBase.end())
            {
                continue;
            }

            for (const auto& derived : it->second)
            {
                if (visited.insert(derived.qualifiedName).second)
                {
                    derivedClasses.push_back(derived.qualifiedName);
                    next.push_back(derived.qualifiedName);
                }

                if (!derived.name.empty() && derived.name != derived.qualifiedName &&
                    visited.insert(derived.name).second)
                {
                    derivedClasses.push_back(derived.name);
                    next.push_back(derived.name);
                }
            }
        }

        frontier = std::move(next);
    }

    return derivedClasses;
}

void AddClassAndShortName(const std::string& c, ankerl::unordered_dense::set<std::string>& visited,
                          std::vector<std::string>& result)
{
    if (c.empty() || !visited.insert(c).second)
    {
        return;
    }
    result.push_back(c);
    const std::string_view shortName = utils::LastScopeSegment(c);
    if (shortName != c)
    {
        if (visited.insert(std::string(shortName)).second)
        {
            result.push_back(std::string(shortName));
        }
    }
}

bool HasNonPrivateMember(const std::string& typeName, const std::string& memberName, const SymbolTable& symbolTable)
{
    const auto syms = symbolTable.FindMemberSymbolPtr(typeName, memberName);
    if (!syms)
    {
        return false;
    }
    for (const auto& s : *syms)
    {
        AccessModifier a = AccessModifier::Public;
        if (std::holds_alternative<FunctionSignature>(s.signature))
        {
            a = s.GetFunction().modifiers.access;
        }
        else if (std::holds_alternative<VariableSignature>(s.signature))
        {
            a = s.GetVariable().modifiers.access;
        }
        if (a != AccessModifier::Private)
        {
            return true;
        }
    }
    return false;
}

std::string FindDeclaringRootClass(const std::string& cleanClass, const std::string& memberName,
                                   const SymbolTable& symbolTable)
{
    std::string rootClass = cleanClass;
    const auto hierarchy = GetInheritedTypeHierarchy(cleanClass, symbolTable);
    for (size_t i = 1; i < hierarchy.size(); ++i)
    {
        if (HasNonPrivateMember(hierarchy[i], memberName, symbolTable))
        {
            rootClass = hierarchy[i];
        }
    }
    return rootClass;
}

void ExpandMixinFromRuleIndex(const rules::RuleIndex& ruleIndex, const MixinExpansionContext& ctx)
{
    auto it = ruleIndex.hostClassesByMixin.find(ctx.cleanClass);
    if (it == ruleIndex.hostClassesByMixin.end() && utils::ContainsScopeSeparator(ctx.cleanClass))
    {
        it = ruleIndex.hostClassesByMixin.find(std::string(LastScopeSegment(ctx.cleanClass)));
    }
    if (it == ruleIndex.hostClassesByMixin.end())
    {
        return;
    }
    for (const auto& host : it->second)
    {
        std::string hostKey = host.qualifiedName.empty() ? host.name : host.qualifiedName;
        AddClassAndShortName(hostKey, ctx.visited, ctx.result);
        if (auto hostSyms = ctx.symbolTable.FindSymbolsPtr(hostKey))
        {
            for (const auto& hs : *hostSyms)
            {
                if (hs.type == SymbolType::Class && std::holds_alternative<ClassSignature>(hs.signature))
                {
                    for (const auto& sibMixin : hs.GetClass().includedMixins)
                    {
                        AddClassAndShortName(sibMixin, ctx.visited, ctx.result);
                    }
                }
            }
        }
    }
}

void ExpandMixinFromSymbolTable(const MixinExpansionContext& ctx)
{
    std::string_view shortKey = LastScopeSegment(ctx.cleanClass);
    ctx.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& hs : symbols)
            {
                if (hs.type != SymbolType::Class || !std::holds_alternative<ClassSignature>(hs.signature))
                {
                    continue;
                }
                const auto& cls = hs.GetClass();
                bool includes = false;
                for (const auto& m : cls.includedMixins)
                {
                    if (m == ctx.cleanClass || LastScopeSegment(m) == shortKey)
                    {
                        includes = true;
                        break;
                    }
                }
                if (includes)
                {
                    std::string hostKey = hs.qualifiedName.empty() ? hs.name : hs.qualifiedName;
                    AddClassAndShortName(hostKey, ctx.visited, ctx.result);
                    for (const auto& sibMixin : cls.includedMixins)
                    {
                        AddClassAndShortName(sibMixin, ctx.visited, ctx.result);
                    }
                }
            }
        });
}

void ExpandMixinHostsAndSiblings(const std::string& cleanClass, const SymbolTable& symbolTable,
                                 ankerl::unordered_dense::set<std::string>& visited, std::vector<std::string>& result)
{
    MixinExpansionContext ctx{cleanClass, symbolTable, visited, result};
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        ExpandMixinFromRuleIndex(*ruleIndex, ctx);
    }
    else
    {
        ExpandMixinFromSymbolTable(ctx);
    }
}

void CollectPrivateCompatibleClasses(const std::string& cleanClass, const SymbolTable& symbolTable,
                                     ankerl::unordered_dense::set<std::string>& visited,
                                     std::vector<std::string>& result)
{
    AddClassAndShortName(cleanClass, visited, result);
    if (!utils::ContainsScopeSeparator(cleanClass))
    {
        for (const auto& ts : symbolTable.FindTypeSymbolsByShortName(cleanClass))
        {
            if ((ts.type == SymbolType::Class || ts.type == SymbolType::Interface) && !ts.qualifiedName.empty())
            {
                AddClassAndShortName(ts.qualifiedName, visited, result);
            }
        }
    }
    ExpandMixinHostsAndSiblings(cleanClass, symbolTable, visited, result);
}

std::vector<std::string> GetCompatibleMemberClasses(const std::string& className, const std::string& memberName,
                                                    AccessModifier access, const SymbolTable& symbolTable)
{
    std::vector<std::string> result;
    const std::string cleanClass = CleanBaseType(className);
    if (cleanClass.empty())
    {
        return result;
    }

    ankerl::unordered_dense::set<std::string> visited;
    if (access == AccessModifier::Private)
    {
        CollectPrivateCompatibleClasses(cleanClass, symbolTable, visited, result);
        return result;
    }

    const std::string rootClass = FindDeclaringRootClass(cleanClass, memberName, symbolTable);
    AddClassAndShortName(rootClass, visited, result);

    for (const auto& d : GetDerivedClasses(rootClass, symbolTable))
    {
        AddClassAndShortName(d, visited, result);
    }

    ExpandMixinHostsAndSiblings(rootClass, symbolTable, visited, result);

    if (access == AccessModifier::Public && rootClass != cleanClass)
    {
        for (const auto& base : GetInheritedTypeHierarchy(rootClass, symbolTable))
        {
            AddClassAndShortName(base, visited, result);
        }
    }

    return result;
}

std::string InferHostClassFromEnclosingMixin(TSNode node, std::string_view sourceCode, const SymbolTable& symbolTable,
                                             ExpressionTypeCache* cache)
{
    const auto ruleIndex = symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return "";
    }
    auto containers = GetEnclosingContainers(node, sourceCode);
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class)
        {
            std::string cName = c.qualifiedName.empty() ? c.name : c.qualifiedName;
            if (cache)
            {
                auto itCached = cache->inferredMixinHostByContainer.find(cName);
                if (itCached != cache->inferredMixinHostByContainer.end())
                {
                    return itCached->second;
                }
            }
            auto it = ruleIndex->hostClassesByMixin.find(cName);
            if (it == ruleIndex->hostClassesByMixin.end() && utils::ContainsScopeSeparator(cName))
            {
                it = ruleIndex->hostClassesByMixin.find(LastScopeSegment(cName));
            }
            if (it != ruleIndex->hostClassesByMixin.end() && !it->second.empty())
            {
                std::string host = it->second.front().qualifiedName.empty() ? it->second.front().name
                                                                            : it->second.front().qualifiedName;
                if (cache)
                {
                    cache->inferredMixinHostByContainer.emplace(std::move(cName), host);
                }
                return host;
            }
            if (cache)
            {
                cache->inferredMixinHostByContainer.emplace(std::move(cName), "");
            }
        }
    }
    return "";
}

TSNode FindMixinInclusionNode(TSNode hostNode, std::string_view mixinName, std::string_view mixinShortName,
                              std::string_view sourceCode)
{
    if (ts_node_is_null(hostNode))
    {
        return {};
    }

    TSNode found{};
    TSNode baseList = parser::FindFirstChildOfType(hostNode, "base_class_list");
    if (!ts_node_is_null(baseList))
    {
        parser::ForEachNamedChildNodeUntil(baseList,
                                           [&](TSNode baseChild)
                                           {
                                               const std::string bText = parser::GetNodeText(baseChild, sourceCode);
                                               if (CleanBaseTypeView(bText) == mixinName || bText == mixinShortName ||
                                                   utils::EndsWithScopedMember(bText, mixinShortName))
                                               {
                                                   found = baseChild;
                                                   return false;
                                               }
                                               return true;
                                           });
        if (!ts_node_is_null(found))
        {
            return found;
        }
    }

    TSNode bodyNode = parser::GetChildByField(hostNode, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        parser::ForEachChildNodeUntil(bodyNode,
                                      [&](TSNode mNode)
                                      {
                                          const std::string mText = parser::GetNodeText(mNode, sourceCode);
                                          if (!mixinShortName.empty() &&
                                              mText.find(mixinShortName) != std::string::npos)
                                          {
                                              found = mNode;
                                              return false;
                                          }
                                          return true;
                                      });
    }
    return found;
}

bool IsDestructorSymbol(const Symbol& sym)
{
    if (sym.type != SymbolType::Function)
    {
        return false;
    }
    return sym.name.starts_with("~");
}

bool IsDestructorDeclaration(const Symbol& sym, std::string_view source)
{
    if (sym.type != SymbolType::Function)
    {
        return false;
    }
    if (sym.name.starts_with('~'))
    {
        return true;
    }
    if (source.empty())
    {
        return false;
    }

    size_t offset = 0;
    for (uint32_t line = 0; line < sym.selectionRange.startLine; ++line)
    {
        offset = source.find('\n', offset);
        if (offset == std::string_view::npos)
        {
            return false;
        }
        ++offset;
    }

    offset += sym.selectionRange.startCharacter;
    if (offset == 0 || offset > source.size())
    {
        return false;
    }

    size_t back = offset;
    while (back > 0 && (source[back - 1] == ' ' || source[back - 1] == '\t'))
    {
        --back;
    }
    return back > 0 && source[back - 1] == '~';
}

bool IsDestructorDeclaration(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.fileUri != ctx.request.fileUri)
    {
        return false;
    }
    return IsDestructorDeclaration(sym, ctx.request.sourceCode);
}

bool HasBaseClassList(TSNode classNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t childCount = ts_node_named_child_count(classNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        if (ts_node_symbol(ts_node_named_child(classNode, i)) == gs.symBaseClassList)
        {
            return true;
        }
    }
    return false;
}

bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode function = parser::FindAncestorWithSymbol(parser::GetParent(node), gs.symFuncDeclaration);
    if (ts_node_is_null(function) ||
        !ts_node_is_null(parser::GetChildByField(function, parser::GetGrammarSymbols().fieldReturnType)))
    {
        return false;
    }

    const TSNode functionName = parser::GetChildByField(function, parser::GetGrammarSymbols().fieldName);
    const TSNode owner = parser::FindAncestorWithSymbol(parser::GetParent(function), gs.symClassDeclaration);
    if (ts_node_is_null(functionName) || ts_node_is_null(owner))
    {
        return false;
    }

    const TSNode className = parser::GetChildByField(owner, parser::GetGrammarSymbols().fieldName);
    if (ts_node_is_null(className) ||
        parser::GetNodeText(className, sourceCode) != parser::GetNodeText(functionName, sourceCode))
    {
        return false;
    }

    return HasBaseClassList(owner);
}

} // namespace angel_lsp::analysis
