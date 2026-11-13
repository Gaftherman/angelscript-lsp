#include "analysis/resolution/DefinitionResolution.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/DefinitionResolutionInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "config/EngineRuleConfig.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::vector<Symbol> TryResolveOverriddenMethod(std::string_view nodeText, const Scope* innerScope,
                                                      const DefinitionQuery& query)
{
    std::string enclosingClass;
    if (innerScope && !innerScope->GetEnclosingClass().empty())
    {
        enclosingClass = std::string(innerScope->GetEnclosingClass());
    }
    else
    {
        enclosingClass = query.symbolTable.FindEnclosingClassName(query.uri, query.line);
    }
    if (enclosingClass.empty())
    {
        return {};
    }

    auto localMethods = query.symbolTable.FindMemberSymbols(enclosingClass, nodeText);
    bool isDecl = false;
    for (const auto& m : localMethods)
    {
        if (m.startLine == query.line)
        {
            isDecl = true;
            break;
        }
    }
    if (!isDecl)
    {
        return {};
    }

    LinkedBaseOptions linkedOpts;
    if (query.engineRules)
    {
        linkedOpts.proxyMap = &query.engineRules->unusedRules.baseClassProxyMap;
        linkedOpts.delegateProps = query.engineRules->unusedRules.baseClassDelegateProperties;
    }
    return FindOverriddenBaseMethodSymbols(enclosingClass, nodeText, query.symbolTable, linkedOpts);
}

static std::optional<SourceRange> TryResolveLocalDeclaration(std::string_view nodeText, const Scope* innerScope,
                                                             const DefinitionQuery& query)
{
    if (!innerScope)
    {
        return std::nullopt;
    }

    for (const auto& def : innerScope->definitions)
    {
        if (def.name == nodeText && def.startLine == query.line && def.startCharacter <= query.character &&
            query.character <= def.endCharacter)
        {
            const bool hasFull = (def.fullEndLine > 0 || def.fullEndCharacter > 0);
            return hasFull
                       ? SourceRange{def.fullStartLine, def.fullStartCharacter, def.fullEndLine, def.fullEndCharacter}
                       : SourceRange{def.startLine, def.startCharacter, def.endLine, def.endCharacter};
        }
    }
    return std::nullopt;
}

DefinitionTargetResult ResolveDefinitionTarget(const DefinitionQuery& query)
{
    DefinitionTargetResult result;
    if (query.sourceCode.empty() || query.identifier.empty() || !IsValidIdentifier(query.identifier))
    {
        return result;
    }

    std::string_view nodeText = query.identifier;

    result.symbols = TryResolveThisKeyword(nodeText, query);
    if (!result.symbols.empty())
    {
        return result;
    }

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    const Scope* innerScope = rootScope ? FindInnermostScope(rootScope.get(), query.line, query.character) : nullptr;

    result.symbols = TryResolveOverriddenMethod(nodeText, innerScope, query);
    if (!result.symbols.empty())
    {
        return result;
    }

    if (auto localRange = TryResolveLocalDeclaration(nodeText, innerScope, query))
    {
        result.localRange = *localRange;
        result.localUri = query.uri;
        return result;
    }

    auto resolveReq = MakeTargetRequest(query.uri, query.sourceCode, TargetPosition{query.line, query.character},
                                        query.symbolTable, query.scopeIndex);
    resolveReq.useCallBindings = true;
    resolveReq.callBindingRevision = query.callBindingRevision;
    if (auto target = ResolveTargetSymbol(resolveReq))
    {
        if (target->kind == TargetKind::Local)
        {
            const auto& def = target->localDef;
            const bool hasFull = (def.fullEndLine > 0 || def.fullEndCharacter > 0);
            result.localRange =
                hasFull ? SourceRange{def.fullStartLine, def.fullStartCharacter, def.fullEndLine, def.fullEndCharacter}
                        : SourceRange{def.startLine, def.startCharacter, def.endLine, def.endCharacter};
            result.localUri = query.uri;
            return result;
        }
        if (target->kind == TargetKind::ClassMember)
        {
            if (target->callDeclaration.has_value())
            {
                result.callDeclaration = target->callDeclaration;
                return result;
            }
            result.symbols = ResolveClassMemberSymbols(*target, innerScope, query);
            if (!result.symbols.empty())
            {
                return result;
            }
        }
        else if (target->kind == TargetKind::NamespaceSymbol || target->kind == TargetKind::EnumMember)
        {
            result.symbols = query.symbolTable.FindSymbols(target->qualifiedName);
            if (result.symbols.empty())
            {
                result.symbols = query.symbolTable.FindSymbols(target->name);
            }
            if (!result.symbols.empty())
            {
                return result;
            }
        }
        else if (target->callDeclaration.has_value())
        {
            result.callDeclaration = target->callDeclaration;
            return result;
        }
        else
        {
            result.symbols = query.symbolTable.FindSymbols(target->name);
            if (result.symbols.empty())
            {
                result.symbols = FindGlobalPropertyAccessors(target->name, query.symbolTable);
            }
            DisambiguateCandidateContainers(result.symbols, innerScope, query);
            if (!result.symbols.empty())
            {
                return result;
            }
        }
    }

    result.symbols = TryResolveDelegateProperty(nodeText, query);
    if (!result.symbols.empty())
    {
        return result;
    }

    result.symbols = TryResolveScopedIdentifier(query);
    if (!result.symbols.empty())
    {
        return result;
    }

    const std::string enclosingNs = FindEnclosingNamespace(query.uri, query.line, query.symbolTable);
    if (!enclosingNs.empty())
    {
        utils::ForEachEnclosingScope(enclosingNs,
                                     [&](std::string_view scope)
                                     {
                                         if (!result.symbols.empty())
                                         {
                                             return;
                                         }
                                         result.symbols = query.symbolTable.FindMemberSymbols(scope, nodeText);
                                     });
        if (!result.symbols.empty())
        {
            DisambiguateCandidateContainers(result.symbols, innerScope, query);
            return result;
        }
    }

    result.symbols = query.symbolTable.FindSymbols(nodeText);
    DisambiguateCandidateContainers(result.symbols, innerScope, query);
    return result;
}

} // namespace angel_lsp::analysis
