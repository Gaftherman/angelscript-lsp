#include "analysis/resolution/ReferenceCountingAccess.h"

#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "analysis/resolution/ReferenceCountingReceiver.h"
#include "analysis/resolution/ReferenceCountingScope.h"
#include "analysis/resolution/ReferenceReceiver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
bool IsConstructorDeclaration(const Symbol& symbol)
{
    return symbol.type == SymbolType::Function && symbol.containerKind == ContainerKind::Class &&
           symbol.name == utils::LastScopeSegment(symbol.containerName);
}

std::span<const IndexedDefaultConstruction> FindDefaultConstructionSites(const LocalReference& typeReference,
                                                                         const Scope& scope)
{
    if (!scope.enclosingContext || !scope.enclosingContext->receivers)
    {
        return {};
    }
    const auto sites = std::ranges::equal_range(scope.enclosingContext->receivers->defaultConstructions,
                                                typeReference.startByte, {}, &IndexedDefaultConstruction::typeByte);
    return {sites.begin(), sites.end()};
}

bool IsDeclarationOrDefinition(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                               const ReferenceCollectionCriteria& criteria)
{
    if (criteria.allDeclRanges.contains({std::string(fileUri), ref.startLine, ref.startCharacter}))
    {
        return true;
    }
    for (const auto& sym : criteria.group)
    {
        if (fileUri == sym.fileUri && ref.startLine == sym.selectionRange.startLine &&
            ref.startCharacter == sym.selectionRange.startCharacter)
        {
            return true;
        }
    }
    for (const auto* s = scope; s != nullptr; s = s->parent)
    {
        for (const auto& def : s->definitions)
        {
            if (def.name == criteria.targetName && def.startLine == ref.startLine &&
                def.startCharacter == ref.startCharacter)
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsInTargetHierarchy(std::string_view encClass, const std::vector<Symbol>& group,
                                const SymbolTable& symbolTable)
{
    std::string_view cleanEnc = CleanBaseTypeView(encClass);
    for (const auto& s : group)
    {
        if (s.containerName.empty())
        {
            continue;
        }
        std::string_view cleanDecl = CleanBaseTypeView(s.containerName);
        if (MatchesDeclOrScope(cleanEnc, cleanDecl))
        {
            return true;
        }
        auto hierarchy = GetInheritedTypeHierarchy(cleanEnc, symbolTable);
        for (const auto& ancestor : hierarchy)
        {
            if (MatchesDeclOrScope(CleanBaseTypeView(ancestor), cleanDecl))
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsLocalShadowingTarget(const Scope* scope, std::string_view name, std::string_view fileUri,
                                   const ReferenceCollectionCriteria& criteria)
{
    const LocalDefinition* localShadow = ResolveInScope(scope, name);
    if (!localShadow)
    {
        return false;
    }
    if (localShadow->kind != LocalDefinitionKind::Parameter && localShadow->kind != LocalDefinitionKind::Variable)
    {
        return false;
    }
    if (criteria.allDeclRanges.contains({std::string(fileUri), localShadow->startLine, localShadow->startCharacter}))
    {
        return false;
    }
    for (const auto& sym : criteria.group)
    {
        if (fileUri == sym.fileUri && localShadow->startLine == sym.selectionRange.startLine &&
            localShadow->startCharacter == sym.selectionRange.startCharacter)
        {
            return false;
        }
    }
    return true;
}

static bool IsCompatibleNonMemberAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                        const ReferenceCollectionCriteria& criteria)
{
    std::string_view encClass =
        GetEnclosingClassName(criteria.symbolTable, fileUri, ref.startLine, criteria.classCache);
    if (encClass.empty() || (!criteria.compatibleClasses.contains(encClass) &&
                             !criteria.compatibleClasses.contains(utils::LastScopeSegment(encClass))))
    {
        return false;
    }
    if (!IsInTargetHierarchy(encClass, criteria.group, criteria.symbolTable))
    {
        return false;
    }
    return !IsLocalShadowingTarget(scope, criteria.targetName, fileUri, criteria);
}

static bool IsValidEnumAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                              const ReferenceCollectionCriteria& criteria)
{
    if (ref.isCall)
    {
        return false;
    }
    if (ref.isMemberAccess)
    {
        const auto qualifier = GetIndexedReferenceQualifier(ref, *scope);
        return MatchesEnumReferenceQualifier(qualifier, criteria.targetEnumContainer, *scope);
    }

    if (!criteria.targetNamespace.empty())
    {
        if (!IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable,
                               criteria.classCache))
        {
            return false;
        }
    }
    else if (fileUri != criteria.request.uri)
    {
        return false;
    }

    const LocalDefinition* localShadow = ResolveInScope(scope, ref.name);
    return !(localShadow && (localShadow->kind == LocalDefinitionKind::Parameter ||
                             localShadow->kind == LocalDefinitionKind::Variable));
}

static bool IsEnclosingClassIncompatible(std::string_view fileUri, uint32_t line,
                                         const ReferenceCollectionCriteria& criteria)
{
    if (criteria.targetAccess == AccessModifier::Private || criteria.targetAccess == AccessModifier::Protected)
    {
        std::string_view encClass = GetEnclosingClassName(criteria.symbolTable, fileUri, line, criteria.classCache);
        if (encClass.empty() || (!criteria.compatibleClasses.contains(encClass) &&
                                 !criteria.compatibleClasses.contains(utils::LastScopeSegment(encClass))))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks whether the target is the class's factory for a bare declarator.
 * @param[in] target Constructor declaration under search.
 * @param[in] lookup Symbol snapshot and default constructor policy of the use site.
 * @return True for the selected declared factory, or false when the engine supplies its own factory.
 */
static bool IsDefaultConstructorTarget(const Symbol& target, const DefaultConstructorLookup& lookup)
{
    const auto& table = lookup.symbolTable;
    if (ClassifyNonInstantiable(target.containerName, table, target.fileUri) != NonInstantiableKind::None)
    {
        return false;
    }
    const auto candidates = table.FindMemberSymbolPtr(target.containerName, target.name);
    if (!candidates)
    {
        return false;
    }
    const Symbol* selected = nullptr;
    for (const auto& candidate : *candidates)
    {
        if (candidate.fileUri != target.fileUri || !IsConstructorDeclaration(candidate))
        {
            continue;
        }
        const auto& parameters = candidate.GetFunction().parameters;
        if (parameters.empty())
        {
            selected = &candidate;
            break;
        }
        if (!selected && (!lookup.alwaysImplementsDefaultConstruct || candidate.isPredefined) &&
            std::ranges::all_of(parameters, [](const auto& p) { return !p.defaultValue.empty(); }))
        {
            selected = &candidate;
        }
    }
    return selected && !selected->GetFunction().modifiers.isDelete && selected->fileUri == target.fileUri &&
           selected->selectionRange == target.selectionRange;
}

static bool CanAccessConstructor(const Scope& scope, const Symbol& constructor, const SymbolTable& table)
{
    const auto access = constructor.GetFunction().modifiers.access;
    const auto accessingClass = scope.GetEnclosingClass();
    if (access == AccessModifier::Public || accessingClass == constructor.containerName)
    {
        return true;
    }
    return access == AccessModifier::Protected && !accessingClass.empty() &&
           std::ranges::any_of(GetInheritedTypeHierarchy(accessingClass, table),
                               [&](const auto& base) { return base == constructor.containerName; });
}

std::span<const IndexedDefaultConstruction> FindDefaultConstructorUses(const LocalReference& ref, const Scope& scope,
                                                                       const Symbol& constructor,
                                                                       const DefaultConstructorLookup& lookup)
{
    if (!ref.isTypeSpecifier || ref.isCall || ref.name != constructor.name || !IsConstructorDeclaration(constructor))
    {
        return {};
    }
    const auto sites = FindDefaultConstructionSites(ref, scope);
    const auto& table = lookup.symbolTable;
    if (sites.empty() ||
        !MatchesWrittenTypeOwner(ref, scope, utils::ParentScopeView(constructor.containerName), table) ||
        !CanAccessConstructor(scope, constructor, table) || !IsDefaultConstructorTarget(constructor, lookup))
    {
        return {};
    }
    return sites;
}

static bool IsValidConstructorAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                     const ReferenceCollectionCriteria& criteria)
{
    if (!scope || !ref.isCall || !CanAccessConstructor(*scope, criteria.group.front(), criteria.symbolTable))
    {
        return false;
    }
    const std::string_view targetNamespace = utils::ParentScopeView(criteria.group.front().containerName);
    const std::string_view qualifier = GetIndexedReferenceQualifier(ref, *scope);
    if (!qualifier.empty())
    {
        return targetNamespace.empty() ? qualifier == utils::k_scopeSeparator
                                       : MatchesReferenceQualifier(qualifier, targetNamespace, *scope);
    }
    if (ref.isMemberAccess ||
        (!targetNamespace.empty() &&
         !IsInsideNamespace(fileUri, ref.startLine, targetNamespace, criteria.symbolTable, criteria.classCache) &&
         !IsUsingNamespaceActive(scope, ref.startByte, targetNamespace)))
    {
        return false;
    }
    return !IsLocalShadowingTarget(scope, ref.name, fileUri, criteria);
}

bool IsValidAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                   const ReferenceCollectionCriteria& criteria)
{
    if (criteria.isEnumConstant)
    {
        return IsValidEnumAccess(fileUri, ref, scope, criteria);
    }
    const bool isConstructor =
        criteria.isFunction && !criteria.group.empty() && IsConstructorDeclaration(criteria.group.front());
    if (isConstructor)
    {
        return IsValidConstructorAccess(fileUri, ref, scope, criteria);
    }
    if (scope && ref.isTypeSpecifier && !criteria.group.empty())
    {
        const auto& target = criteria.group.front();
        if (target.type == SymbolType::Class || target.type == SymbolType::Interface)
        {
            return MatchesWrittenTypeOwner(ref, *scope, target.containerName, criteria.symbolTable);
        }
    }
    const std::string_view qualifier = scope ? GetIndexedReferenceQualifier(ref, *scope) : std::string_view{};
    if (!criteria.compatibleClasses.empty())
    {
        if (IsEnclosingClassIncompatible(fileUri, ref.startLine, criteria))
        {
            return false;
        }
        if (criteria.isTypeTarget)
        {
            if (!qualifier.empty())
            {
                return MatchesClassTypeQualifier(qualifier, scope, ref.startByte, criteria);
            }
            return IsCompatibleNonMemberAccess(fileUri, ref, scope, criteria);
        }
        return ref.isMemberAccess ? IsCompatibleMemberAccess(fileUri, ref, scope, criteria)
                                  : IsCompatibleNonMemberAccess(fileUri, ref, scope, criteria);
    }

    if (ref.isMemberAccess || !qualifier.empty())
    {
        if (criteria.targetNamespace.empty())
        {
            return !ref.isMemberAccess && qualifier == utils::k_scopeSeparator;
        }
        return scope && MatchesReferenceQualifier(qualifier, criteria.targetNamespace, *scope);
    }

    if (!criteria.targetNamespace.empty())
    {
        if (!IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable,
                               criteria.classCache) &&
            !IsUsingNamespaceActive(scope, ref.startByte, criteria.targetNamespace))
        {
            return false;
        }
    }
    else if (IsGlobalTypeShadowed(scope, criteria))
    {
        return false;
    }

    return !IsLocalShadowingTarget(scope, ref.name, fileUri, criteria);
}
} // namespace angel_lsp::analysis
