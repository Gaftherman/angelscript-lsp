#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::string_view FindEnclosingClassOrInterface(const CallSiteFact& site, const Scope* scope,
                                                      const NodeIndex* nodeIndex = nullptr,
                                                      std::string_view sourceCode = {})
{
    if (scope)
    {
        if (auto enc = scope->GetEnclosingClass(); !enc.empty())
        {
            return enc;
        }
    }
    if (!site.enclosingClassName.empty())
    {
        return site.enclosingClassName;
    }
    if (nodeIndex && !sourceCode.empty())
    {
        if (const auto* span = nodeIndex->FindInnermostClassSpan(site.bytes.startByte))
        {
            if (span->nameStartByte < span->nameEndByte && span->nameEndByte <= sourceCode.size())
            {
                return sourceCode.substr(span->nameStartByte, span->nameEndByte - span->nameStartByte);
            }
        }
    }
    return {};
}

static std::string_view GetScopeQualifierPrefix(std::string_view name) noexcept
{
    return utils::ParentScopeView(name);
}

static bool IsInsideTargetClassOrSubclass(const CallSiteFact& site, std::string_view targetClass, const Scope* scope,
                                          const DiagnosticContext& ctx)
{
    if (targetClass.empty())
    {
        return false;
    }
    const std::string_view enclosing =
        FindEnclosingClassOrInterface(site, scope, ctx.request.nodeIndex, ctx.request.sourceCode);
    if (enclosing.empty())
    {
        return false;
    }
    if (enclosing == targetClass || LastScopeSegment(enclosing) == targetClass)
    {
        return true;
    }
    const auto hierarchy = GetInheritedTypeHierarchy(enclosing, ctx.request.symbolTable);
    return std::any_of(hierarchy.begin(), hierarchy.end(), [targetClass](std::string_view ancestor)
                       { return ancestor == targetClass || LastScopeSegment(ancestor) == targetClass; });
}

static bool IsClassOrInterfaceSymbol(const Symbol& s) noexcept
{
    return s.containerKind == ContainerKind::Class || s.containerKind == ContainerKind::Interface;
}

static void FilterScopedCandidatesByCallerContext(std::vector<Symbol>& candidates, bool insideTarget)
{
    if (insideTarget || candidates.empty())
    {
        return;
    }
    const bool hasNamespaceCandidates = std::any_of(candidates.begin(), candidates.end(), [](const Symbol& s)
                                                    { return s.containerKind == ContainerKind::Namespace; });

    if (hasNamespaceCandidates)
    {
        std::erase_if(candidates, IsClassOrInterfaceSymbol);
    }
}

static bool ResolveScopedCallee(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx,
                                CalleeResolution& res)
{
    if (NamesAType(site.calleeText, ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return true;
    }
    res.candidates = CollectScopedFunctionCandidates(site.calleeText, ctx.request.symbolTable);

    const std::string_view qualifier = GetScopeQualifierPrefix(site.calleeText);
    const bool insideTarget = IsInsideTargetClassOrSubclass(site, qualifier, scope, ctx);
    FilterScopedCandidatesByCallerContext(res.candidates, insideTarget);

    res.candidatesAreFreeFunctions = true;
    if (!res.candidates.empty() || CheckScopedNonCallableVariable(site, ctx, res))
    {
        return true;
    }
    return true;
}

static bool ResolveEnclosingClassCallee(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx,
                                        CalleeResolution& res)
{
    std::string_view enclosingClass;
    bool isConst = false;
    if (scope)
    {
        if (auto enc = scope->GetEnclosingClass(); !enc.empty())
        {
            enclosingClass = enc;
            isConst = scope->isConstMethod;
        }
    }
    if (enclosingClass.empty())
    {
        enclosingClass = FindEnclosingClassOrInterface(site, scope, ctx.request.nodeIndex, ctx.request.sourceCode);
        isConst = site.isEnclosingMethodConst;
    }
    if (enclosingClass.empty())
    {
        return false;
    }
    res.candidates =
        FindMethodCandidates(enclosingClass, site.calleeName, ctx.request.symbolTable, ctx.request.exprCache);
    if (!res.candidates.empty())
    {
        res.candidatesAreFreeFunctions = false;
        res.isUnqualifiedClassCall = true;
        res.isReceiverConst = isConst;
        FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
        ApplyTemplateSubstitutions(res.candidates, enclosingClass, {}, ctx.request.symbolTable);
        return true;
    }
    if (auto callableSym = TryResolveMemberCallableFuncdef(enclosingClass, site.calleeName, ctx.request.symbolTable,
                                                           ctx.request.exprCache))
    {
        res.candidates = {std::move(*callableSym)};
        res.candidatesAreFreeFunctions = true;
        res.isUnqualifiedClassCall = true;
        res.shouldCheck = true;
        return true;
    }
    return CheckMemberNonCallableVariable(site, enclosingClass, ctx, res);
}

static CalleeResolution ResolveIdentifierCallee(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    CalleeResolution res;
    if (site.calleeText.empty())
    {
        res.shouldCheck = false;
        return res;
    }

    res.reportedName = site.calleeName;

    if (site.calleeKind == CalleeKind::Scoped)
    {
        ResolveScopedCallee(site, scope, ctx, res);
        return res;
    }

    if (auto callableSym =
            TryResolveCallableFuncdef(site.calleeName, scope, ctx.request.symbolTable, site.calleeNameBytes.startByte))
    {
        res.candidates = {std::move(*callableSym)};
        res.candidatesAreFreeFunctions = true;
        res.shouldCheck = true;
        return res;
    }

    if (CheckLocalNonCallable(site, scope, ctx, res) || ResolveEnclosingClassCallee(site, scope, ctx, res))
    {
        return res;
    }

    if (NamesAType(site.calleeName, ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.candidatesAreFreeFunctions = true;
    const FreeLookupContext freeCtx{site.bytes.startByte,       ctx.request.sourceCode,
                                    ctx.request.fileUri,        ctx.request.predefinedFileExtension,
                                    ctx.request.symbolTable,    ctx.request.GetRuleIndex(),
                                    ctx.request.moduleFileUris, ctx.request.nodeIndex};
    res.candidates = FindFreeCandidates(site.calleeText, freeCtx);
    if (!res.candidates.empty() || CheckGlobalNonCallableVariable(site, ctx, res))
    {
        return res;
    }

    return res;
}

CalleeResolution ResolveCalleeCandidates(const CallSiteFact& site, const Scope* scope, DiagnosticContext& ctx)
{
    if (site.isMemberCall || site.calleeKind == CalleeKind::Member)
    {
        return ResolveMemberCallee(site, scope, ctx);
    }
    if (site.calleeKind == CalleeKind::Scoped || site.calleeKind == CalleeKind::Identifier)
    {
        return ResolveIdentifierCallee(site, scope, ctx);
    }
    CalleeResolution res;
    res.shouldCheck = false;
    return res;
}

} // namespace angel_lsp::analysis
