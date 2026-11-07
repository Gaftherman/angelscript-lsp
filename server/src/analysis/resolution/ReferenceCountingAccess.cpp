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
    std::string cleanEnc = CleanBaseType(encClass);
    for (const auto& s : group)
    {
        if (s.containerName.empty())
        {
            continue;
        }
        std::string cleanDecl = CleanBaseType(s.containerName);
        if (MatchesDeclOrScope(cleanEnc, cleanDecl))
        {
            return true;
        }
        auto hierarchy = GetInheritedTypeHierarchy(cleanEnc, symbolTable);
        for (const auto& ancestor : hierarchy)
        {
            if (MatchesDeclOrScope(CleanBaseType(ancestor), cleanDecl))
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

static bool MatchesTargetNamespaceQualifier(const LocalReference& ref, const Scope* scope,
                                            const ReferenceCollectionCriteria& criteria)
{
    const auto qualifier = GetIndexedReferenceQualifier(ref, *scope);
    return MatchesReferenceQualifier(qualifier, criteria.targetNamespace, *scope);
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

static bool IsValidConstructorAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                                     const ReferenceCollectionCriteria& criteria)
{
    if (IsEnclosingClassIncompatible(fileUri, ref.startLine, criteria))
    {
        return false;
    }
    if (ref.isMemberAccess)
    {
        return MatchesTargetNamespaceQualifier(ref, scope, criteria);
    }
    if (!criteria.targetNamespace.empty() &&
        !IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable, criteria.classCache))
    {
        return false;
    }
    if (IsLocalShadowingTarget(scope, ref.name, fileUri, criteria))
    {
        return false;
    }
    return (ref.isCall && MatchesCallArguments(ref, criteria)) || (ref.isTypeSpecifier && criteria.minArgs == 0);
}

bool IsValidAccess(std::string_view fileUri, const LocalReference& ref, const Scope* scope,
                   const ReferenceCollectionCriteria& criteria)
{
    if (criteria.isEnumConstant)
    {
        return IsValidEnumAccess(fileUri, ref, scope, criteria);
    }
    const bool isConstructor = criteria.isFunction && !criteria.group.empty() &&
                               criteria.group.front().name == criteria.group.front().containerName;
    if (isConstructor)
    {
        return IsValidConstructorAccess(fileUri, ref, scope, criteria);
    }
    if (!criteria.compatibleClasses.empty())
    {
        if (IsEnclosingClassIncompatible(fileUri, ref.startLine, criteria))
        {
            return false;
        }
        return ref.isMemberAccess ? IsCompatibleMemberAccess(fileUri, ref, scope, criteria)
                                  : IsCompatibleNonMemberAccess(fileUri, ref, scope, criteria);
    }

    if (ref.isMemberAccess)
    {
        return MatchesTargetNamespaceQualifier(ref, scope, criteria);
    }

    if (!criteria.targetNamespace.empty())
    {
        if (!IsInsideNamespace(fileUri, ref.startLine, criteria.targetNamespace, criteria.symbolTable,
                               criteria.classCache))
        {
            return false;
        }
    }

    return !IsLocalShadowingTarget(scope, ref.name, fileUri, criteria);
}
} // namespace angel_lsp::analysis
