#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
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

    if (!FindSymbolsInScope(ctx.request.symbolTable, containers, std::string(name), usings).empty())
    {
        return true;
    }

    const std::string getterName = "get_" + std::string(name);
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
        return;
    }

    if (IsKnownCallee(calleeName, call, cctx, ctx))
    {
        return;
    }

    ctx.EmitAtRange(call.calleeRange, "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
}

} // namespace angel_lsp::analysis
