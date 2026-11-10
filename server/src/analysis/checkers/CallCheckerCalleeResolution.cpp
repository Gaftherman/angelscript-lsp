#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static std::string FindEnclosingClassOrInterface(TSNode node, std::string_view sourceCode,
                                                 const Scope* scopeRoot = nullptr, const NodeIndex* nodeIndex = nullptr)
{
    if (scopeRoot)
    {
        TSPoint pt = ts_node_start_point(node);
        if (const Scope* sc = FindInnermostScope(scopeRoot, pt.row, pt.column))
        {
            if (auto enc = sc->GetEnclosingClass(); !enc.empty())
            {
                return std::string(enc);
            }
        }
    }
    for (const auto& container : GetEnclosingContainers(node, sourceCode, nodeIndex))
    {
        if (container.kind == ContainerKind::Class || container.kind == ContainerKind::Interface)
        {
            return container.qualifiedName.empty() ? container.name : container.qualifiedName;
        }
    }
    return "";
}

static std::string_view GetScopeQualifierPrefix(std::string_view name) noexcept
{
    return utils::ParentScopeView(name);
}

static bool IsInsideTargetClassOrSubclass(TSNode callNode, std::string_view targetClass, std::string_view sourceCode,
                                          const SymbolTable& table, const Scope* scopeRoot = nullptr,
                                          const NodeIndex* nodeIndex = nullptr)
{
    if (targetClass.empty())
    {
        return false;
    }
    const std::string enclosing = FindEnclosingClassOrInterface(callNode, sourceCode, scopeRoot, nodeIndex);
    if (enclosing.empty())
    {
        return false;
    }
    if (enclosing == targetClass || LastScopeSegment(enclosing) == targetClass)
    {
        return true;
    }
    const auto hierarchy = GetInheritedTypeHierarchy(enclosing, table);
    return std::any_of(hierarchy.begin(), hierarchy.end(), [targetClass](const std::string& ancestor)
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

static bool ResolveScopedCallee(std::string_view written, const std::string& shortName,
                                const CallValidationContext& valCtx, CalleeResolution& res)
{
    if (NamesAType(std::string(written), valCtx.ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return true;
    }
    res.candidates = CollectScopedFunctionCandidates(written, valCtx.ctx.request.symbolTable);

    const std::string_view qualifier = GetScopeQualifierPrefix(written);
    const bool insideTarget = IsInsideTargetClassOrSubclass(
        valCtx.callNode, qualifier, valCtx.request.sourceCode, valCtx.ctx.request.symbolTable,
        valCtx.ctx.request.scopeRoot.get(), valCtx.ctx.request.nodeIndex);
    FilterScopedCandidatesByCallerContext(res.candidates, insideTarget);

    res.candidatesAreFreeFunctions = true;
    if (!res.candidates.empty() || CheckScopedNonCallableVariable(written, shortName, valCtx, res))
    {
        return true;
    }
    return true;
}

static bool ResolveEnclosingClassCallee(std::string_view written, const CallValidationContext& valCtx,
                                        CalleeResolution& res)
{
    std::string enclosingClass;
    bool isConst = false;
    if (valCtx.ctx.request.scopeRoot)
    {
        TSPoint pt = ts_node_start_point(valCtx.callNode);
        if (const Scope* sc = FindInnermostScope(valCtx.ctx.request.scopeRoot.get(), pt.row, pt.column))
        {
            if (auto enc = sc->GetEnclosingClass(); !enc.empty())
            {
                enclosingClass = std::string(enc);
                isConst = sc->isConstMethod;
            }
        }
    }
    if (enclosingClass.empty())
    {
        enclosingClass =
            FindEnclosingClassOrInterface(valCtx.callNode, valCtx.request.sourceCode,
                                          valCtx.ctx.request.scopeRoot.get(), valCtx.ctx.request.nodeIndex);
        isConst = IsEnclosingMethodConst(valCtx.callNode);
    }
    if (enclosingClass.empty())
    {
        return false;
    }
    res.candidates = FindMethodCandidates(enclosingClass, std::string(written), valCtx.ctx.request.symbolTable,
                                          valCtx.ctx.request.exprCache);
    if (!res.candidates.empty())
    {
        res.candidatesAreFreeFunctions = false;
        res.isUnqualifiedClassCall = true;
        res.isReceiverConst = isConst;
        FilterMethodCandidatesConstness(res.candidates, res.isReceiverConst);
        ApplyTemplateSubstitutions(res.candidates, enclosingClass, {}, valCtx.ctx.request.symbolTable);
        return true;
    }
    if (auto callableSym = TryResolveMemberCallableFuncdef(
            enclosingClass, std::string(written), valCtx.ctx.request.symbolTable, valCtx.ctx.request.exprCache))
    {
        res.candidates = {std::move(*callableSym)};
        res.candidatesAreFreeFunctions = true;
        res.isUnqualifiedClassCall = true;
        res.shouldCheck = true;
        return true;
    }
    return CheckMemberNonCallableVariable(enclosingClass, std::string(written), valCtx, res);
}

static CalleeResolution ResolveIdentifierCallee(const CallValidationContext& valCtx)
{
    CalleeResolution res;
    const std::string written(parser::NodeText(valCtx.callee, valCtx.request.sourceCode));
    if (written.empty())
    {
        res.shouldCheck = false;
        return res;
    }

    const std::string shortName = std::string(LastScopeSegment(written));
    res.reportedName = shortName;

    if (HasScopeQualifier(written))
    {
        ResolveScopedCallee(written, shortName, valCtx, res);
        return res;
    }

    if (auto callableSym = TryResolveCallableFuncdef(shortName, valCtx.scope, valCtx.ctx.request.symbolTable,
                                                     ts_node_start_byte(valCtx.callee)))
    {
        res.candidates = {std::move(*callableSym)};
        res.candidatesAreFreeFunctions = true;
        res.shouldCheck = true;
        return res;
    }

    if (CheckLocalNonCallable(shortName, valCtx, res) || ResolveEnclosingClassCallee(written, valCtx, res))
    {
        return res;
    }

    if (NamesAType(shortName, valCtx.ctx.request.symbolTable))
    {
        res.shouldCheck = false;
        return res;
    }

    res.candidatesAreFreeFunctions = true;
    const FreeLookupContext freeCtx{valCtx.callNode,
                                    valCtx.request.sourceCode,
                                    valCtx.ctx.request.fileUri,
                                    valCtx.ctx.request.predefinedFileExtension,
                                    valCtx.ctx.request.symbolTable,
                                    valCtx.ctx.request.GetRuleIndex(),
                                    valCtx.ctx.request.moduleFileUris,
                                    valCtx.request.nodeIndex};
    res.candidates = FindFreeCandidates(written, freeCtx);
    if (!res.candidates.empty() || CheckGlobalNonCallableVariable(written, shortName, valCtx, res))
    {
        return res;
    }

    return res;
}

CalleeResolution ResolveCalleeCandidates(const CallValidationContext& valCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol calleeSym = ts_node_symbol(valCtx.callee);
    if (calleeSym == gs.symMemberExpression)
    {
        return ResolveMemberCallee(valCtx);
    }
    const std::string_view calleeType = parser::GetNodeType(valCtx.callee);
    if (calleeSym == gs.symScopedIdentifier || calleeSym == gs.symIdentifier || calleeSym == gs.tokFunction ||
        parser::keywords::IsKeyword(calleeType))
    {
        return ResolveIdentifierCallee(valCtx);
    }
    CalleeResolution res;
    res.shouldCheck = false;
    return res;
}

} // namespace angel_lsp::analysis
