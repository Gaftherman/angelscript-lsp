#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/AccessorNames.h"
#include "utils/ScopedNameUtils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static bool CheckNamespaceOrClassMembers(std::string_view name, const rules::ContainerMembers& members, bool reqKeyword)
{
    const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
    return accSet.contains(name) || members.allMemberNames.contains(name);
}

static bool IsEnclosingContainerCallee(std::string_view name, uint32_t byteOffset,
                                       const NamespaceCallCheckContext& cctx, const DiagnosticContext& ctx)
{
    if (!cctx.nodeIndex)
    {
        return false;
    }
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    const auto containers = cctx.nodeIndex->CollectEnclosingContainers(byteOffset, cctx.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string_view nsName = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                            : std::string_view(container.qualifiedName);
            if (CheckNamespaceOrClassMembers(name, ruleIndex.NamespaceMembers(nsName), reqKeyword))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            if (CheckNamespaceOrClassMembers(name, ruleIndex.ClassMembers(typeName), reqKeyword))
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsCalleeAccessorProperty(std::string_view name, uint32_t byteOffset, const NamespaceCallCheckContext& cctx,
                                     const DiagnosticContext& ctx)
{
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const auto& globalAccessors = ctx.request.RequiresAccessorKeyword() ? ruleIndex.keywordGlobalAccessorPropertyNames
                                                                        : ruleIndex.globalAccessorPropertyNames;
    if (globalAccessors.contains(name) || IsEnclosingContainerCallee(name, byteOffset, cctx, ctx))
    {
        return true;
    }

    return !FindGlobalPropertyAccessors(name, ctx.request.symbolTable, ctx.request.RequiresAccessorKeyword()).empty();
}

static bool IsSpecialContextCallee(std::string_view name, const CallSiteFact& call,
                                   const NamespaceCallCheckContext& cctx)
{
    if (IsReservedKeyword(name))
    {
        return true;
    }

    if (cctx.nodeIndex && cctx.nodeIndex->IsInsideError(call.bytes.startByte))
    {
        return true;
    }

    if (name == "super")
    {
        return call.insideClass && !call.enclosingClassName.empty() &&
               call.enclosingClassName == call.enclosingFunctionName;
    }

    return false;
}

static bool IsKnownCalleeSymbol(std::string_view name, std::string_view getterName, const DiagnosticContext& ctx)
{
    return ctx.request.IsRegisteredSymbol(name) || ctx.request.symbolTable.HasSymbol(name) ||
           ctx.request.IsRegisteredSymbol(getterName) || ctx.request.symbolTable.HasSymbol(getterName);
}

static bool IsKnownCallee(std::string_view name, const CallSiteFact& call, const NamespaceCallCheckContext& cctx,
                          const DiagnosticContext& ctx)
{
    if (IsSpecialContextCallee(name, call, cctx))
    {
        return true;
    }

    const Scope* scope = ctx.request.scopeRoot ? FindEnclosingScope(ctx.request.scopeRoot.get(), call.range.startLine,
                                                                    call.range.startCharacter)
                                               : nullptr;
    if (scope && ResolveInScope(scope, name))
    {
        return true;
    }

    const auto containers = cctx.nodeIndex
                                ? cctx.nodeIndex->CollectEnclosingContainers(call.bytes.startByte, cctx.sourceCode)
                                : std::vector<ContainerInfo>{};
    const auto usings = cctx.nodeIndex ? cctx.nodeIndex->CollectUsingNamespaces(cctx.sourceCode, call.bytes.startByte)
                                       : GetUsingNamespaces(ctx.request);

    if (!FindSymbolsInScope(ctx.request.symbolTable, containers, name, usings).empty())
    {
        return true;
    }

    char getterBuf[512];
    const std::string_view getterName = utils::ComposeGetterName(name, getterBuf);
    if (!FindSymbolsInScope(ctx.request.symbolTable, containers, getterName, usings).empty())
    {
        return true;
    }

    if (IsKnownCalleeSymbol(name, getterName, ctx))
    {
        return true;
    }

    if (IsCalleeAccessorProperty(name, call.bytes.startByte, cctx, ctx))
    {
        return true;
    }

    return call.insideMixin;
}

static bool IsKnownDirectGlobalCallee(std::string_view identView, const DiagnosticContext& ctx)
{
    if (ctx.request.IsRegisteredSymbol(identView))
    {
        return true;
    }
    if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(identView))
    {
        for (const auto& sym : *symsPtr)
        {
            if (sym.containerName.empty() && sym.type != SymbolType::CallReference)
            {
                return true;
            }
        }
    }
    if (ctx.request.scopeRoot)
    {
        if (ResolveInScope(ctx.request.scopeRoot.get(), identView) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Namespaces a qualifier written at a call site may name: itself, then relative to each enclosing
 *        namespace and each `using namespace` that applies at the call.
 * @param[in] qualifier Qualifier text such as `A::B`.
 * @param[in] globalQualified True when the callee started with `::`, which rules out relative lookup.
 * @param[in] call Call site whose position selects the enclosing namespaces.
 * @param[in] cctx Call-check context carrying the node index and source text.
 * @return Candidate namespace paths, most specific first.
 */
static std::vector<std::string> QualifierCandidates(std::string_view qualifier, bool globalQualified,
                                                    const CallSiteFact& call, const NamespaceCallCheckContext& cctx)
{
    std::vector<std::string> candidates{std::string(qualifier)};
    if (globalQualified || !cctx.nodeIndex)
    {
        return candidates;
    }
    for (const auto& container : cctx.nodeIndex->CollectEnclosingContainers(call.bytes.startByte, cctx.sourceCode))
    {
        if (container.kind != ContainerKind::Namespace)
        {
            continue;
        }
        std::string_view path = container.qualifiedName.empty() ? std::string_view(container.name)
                                                                : std::string_view(container.qualifiedName);
        for (; !path.empty(); path = utils::ParentScopeView(path))
        {
            candidates.push_back(utils::JoinScopedName(path, qualifier));
        }
    }
    for (const auto& imported : cctx.nodeIndex->CollectUsingNamespaces(cctx.sourceCode, call.bytes.startByte))
    {
        candidates.push_back(utils::JoinScopedName(imported, qualifier));
    }
    return candidates;
}

/**
 * @brief Reports `A::f()` when `A` resolves to a namespace that declares no `f`.
 *
 * Qualifiers that are not namespaces (classes, enums) are left to the member checks, and so is any
 * qualifier that shares its name with a type, because `A::f()` may then name a static member.
 *
 * @param[in] calleeName Callee text without a leading `::`.
 * @param[in] globalQualified True when the callee started with `::`.
 * @param[in] call Call site being checked.
 * @param[in] cctx Call-check context carrying the node index and source text.
 * @param[in,out] ctx Diagnostic context receiving the error.
 */
static void CheckQualifiedCallee(std::string_view calleeName, bool globalQualified, const CallSiteFact& call,
                                 const NamespaceCallCheckContext& cctx, DiagnosticContext& ctx)
{
    const std::string_view qualifier = utils::ParentScopeView(calleeName);
    const std::string_view member = utils::LastScopeSegment(calleeName);
    if (qualifier.empty() || member.empty() || call.insideMixin || IsSpecialContextCallee(member, call, cctx) ||
        ctx.request.IsRegisteredSymbol(calleeName) ||
        !ctx.request.symbolTable.FindTypeSymbolsByShortName(utils::LastScopeSegment(qualifier)).empty())
    {
        return;
    }
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    bool qualifierIsNamespace = false;
    for (const std::string& candidate : QualifierCandidates(qualifier, globalQualified, call, cctx))
    {
        const auto found = ruleIndex.byNamespace.find(candidate);
        if (found == ruleIndex.byNamespace.end())
        {
            continue;
        }
        qualifierIsNamespace = true;
        if (found->second.allMemberNames.contains(member))
        {
            return;
        }
    }
    if (qualifierIsNamespace)
    {
        ctx.EmitAtRange(call.calleeRange, "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
    }
}

void CheckCallExpression(const CallSiteFact& call, const NamespaceCallCheckContext& cctx, DiagnosticContext& ctx)
{
    std::string_view calleeName = call.calleeText;
    if (call.isMemberCall || calleeName.empty() || calleeName.find('.') != std::string_view::npos)
    {
        return;
    }

    if (utils::HasLeadingGlobalPrefix(calleeName))
    {
        const std::string_view identView = utils::StripGlobalPrefix(calleeName);
        if (!utils::ContainsScopeSeparator(identView))
        {
            if (!identView.empty() && !IsKnownDirectGlobalCallee(identView, ctx))
            {
                ctx.EmitAtRange(call.calleeRange, "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
            }
            return;
        }
        calleeName = utils::StripGlobalPrefix(calleeName);
    }

    if (utils::ContainsScopeSeparator(calleeName))
    {
        CheckQualifiedCallee(calleeName, utils::HasLeadingGlobalPrefix(call.calleeText), call, cctx, ctx);
        return;
    }

    if (IsKnownCallee(calleeName, call, cctx, ctx))
    {
        return;
    }

    ctx.EmitAtRange(call.calleeRange, "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
}

} // namespace angel_lsp::analysis
