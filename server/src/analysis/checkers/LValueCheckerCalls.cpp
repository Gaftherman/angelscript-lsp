#include "analysis/checkers/LValueChecker.h"
#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/index/LValueSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Collects candidate method symbols for a member expression callee.
 * @param[in] callSite Pre-extracted call site fact.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectMemberCandidates(const LValueCallSiteFact& callSite, const CallCandidateContext& cctx,
                                    std::vector<Symbol>& candidates)
{
    if (callSite.memberName.empty() || callSite.objectBytes.endByte == 0)
    {
        return;
    }
    const std::string objType =
        ResolveExpressionType(callSite.objectBytes, ExpressionTypeContext(cctx.scope, cctx.ctx));
    const auto cleanObj = CleanBaseType(objType);
    if (cleanObj.empty())
    {
        return;
    }
    for (const auto& typeName : GetInheritedTypeHierarchy(cleanObj, cctx.ctx.request.symbolTable))
    {
        if (auto found = cctx.ctx.request.symbolTable.FindMemberSymbolPtr(typeName, callSite.memberName))
        {
            for (const auto& sym : *found)
            {
                if (sym.type == SymbolType::Function)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }
}

/**
 * @brief Collects candidate symbols for a scoped identifier callee.
 * @param[in] callSite Pre-extracted call site fact.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectScopedCandidates(const LValueCallSiteFact& callSite, const CallCandidateContext& cctx,
                                    std::vector<Symbol>& candidates)
{
    if (auto found = cctx.ctx.request.symbolTable.FindSymbolsPtr(callSite.calleeText))
    {
        for (const auto& sym : *found)
        {
            if (sym.type == SymbolType::Function)
            {
                candidates.push_back(sym);
            }
        }
    }
}

/**
 * @brief Collects candidate symbols for an unqualified identifier callee.
 * @param[in] callSite Pre-extracted call site fact.
 * @param[in] cctx Call candidate context holding request, scope, and diagnostic context.
 * @param[out] candidates Vector of candidate symbols to append to.
 */
static void CollectIdentifierCandidates(const LValueCallSiteFact& callSite, const CallCandidateContext& cctx,
                                        std::vector<Symbol>& candidates)
{
    std::vector<ContainerInfo> containers;
    if (cctx.request.nodeIndex)
    {
        containers = cctx.request.nodeIndex->CollectLookupContainers(
            callSite.calleeBytes.startByte, callSite.calleeBytes.endByte, 0, cctx.request.sourceCode);
    }
    for (const auto& sym : FindSymbolsInScope(cctx.ctx.request.symbolTable, containers, callSite.calleeText))
    {
        if (sym.type == SymbolType::Function)
        {
            candidates.push_back(sym);
        }
    }
    if (!candidates.empty())
    {
        return;
    }
    for (const auto& c : containers)
    {
        if (c.kind == ContainerKind::Class || c.kind == ContainerKind::Interface)
        {
            const auto hierarchy = GetInheritedTypeHierarchy(c.qualifiedName.empty() ? c.name : c.qualifiedName,
                                                             cctx.ctx.request.symbolTable);
            for (const auto& cls : hierarchy)
            {
                if (auto found = cctx.ctx.request.symbolTable.FindMemberSymbolPtr(cls, callSite.calleeText))
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
            }
            break;
        }
    }
}

/**
 * @brief Validate candidate return types.
 * @param[in] range Diagnostic target range of the call expression.
 * @param[in] candidates Candidate symbols or overloads.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void ValidateCandidateReturnTypes(const SourceRange& range, const std::vector<Symbol>& candidates,
                                         DiagnosticContext& ctx)
{
    if (candidates.empty())
    {
        ctx.EmitAtRange(range, "as-err-assign-non-ref-call");
        return;
    }

    bool allVoid = true;
    bool anyReference = false;

    for (const auto& sym : candidates)
    {
        if (!std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }

        const auto& fn = sym.GetFunction();
        const auto retClean = CleanBaseType(fn.returnType);
        if (fn.returnTypeKind != TypeKind::Void && retClean != "void")
        {
            allVoid = false;
        }

        if (fn.modifiers.isReturnReference || IsReferenceTypeView(fn.returnType))
        {
            anyReference = true;
        }
    }

    if (allVoid)
    {
        ctx.EmitAtRange(range, "as-err-assign-void");
    }
    else if (!anyReference)
    {
        ctx.EmitAtRange(range, "as-err-assign-non-ref-call");
    }
}

/**
 * @brief Validates whether a call expression target is a modifiable reference l-value.
 * @param[in] callSite Pre-extracted call site fact.
 * @param[in] request Contextual request bundle containing source text and parameters.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckCallLValue(const LValueCallSiteFact& callSite, const AstCheckContext& request, const Scope* scope,
                     DiagnosticContext& ctx)
{
    const CallCandidateContext cctx{request, scope, ctx};
    std::vector<Symbol> candidates;

    if (callSite.calleeKind == LValueCalleeKind::Member)
    {
        CollectMemberCandidates(callSite, cctx, candidates);
    }
    else if (callSite.calleeKind == LValueCalleeKind::Scoped)
    {
        CollectScopedCandidates(callSite, cctx, candidates);
    }
    else if (callSite.calleeKind == LValueCalleeKind::Identifier)
    {
        CollectIdentifierCandidates(callSite, cctx, candidates);
    }

    ValidateCandidateReturnTypes(callSite.range, candidates, ctx);
}

} // namespace angel_lsp::analysis
