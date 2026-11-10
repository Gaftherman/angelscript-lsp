#include "analysis/checkers/CallChecker.h"

#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SemanticNodes.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsDelegateConstructionPattern(TSNode argumentList, std::string_view calleeName, std::string_view sourceCode)
{
    const auto argNodes = GetArgumentNodes(argumentList);
    if (argNodes.size() != 1)
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode arg = UnwrapParenNode(argNodes[0]);
    if (ts_node_symbol(arg) != gs.symMemberExpression)
    {
        return false;
    }
    TSNode objNode = ts_node_child_by_field_id(arg, gs.fieldObject);
    TSNode memberNode = ts_node_child_by_field_id(arg, gs.fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memberNode))
    {
        return false;
    }
    const std::string_view objText = parser::NodeText(objNode, sourceCode);
    const std::string_view memberText = parser::NodeText(memberNode, sourceCode);
    return objText == "this" && memberText == calleeName;
}

static bool HasFuncdefSymbol(std::string_view name, const SymbolTable& table)
{
    if (const auto syms = table.FindSymbolsPtr(name))
    {
        if (std::any_of(syms->begin(), syms->end(), [](const Symbol& s) { return s.type == SymbolType::Funcdef; }))
        {
            return true;
        }
    }
    return table.HasFuncdefByShortName(LastScopeSegment(name));
}

static bool CheckArgumentCount(const CalleeResolution& calleeRes, uint32_t argumentCount,
                               const CallValidationContext& valCtx)
{
    const CandidateSet judged = JudgeAgainst(calleeRes.candidates, argumentCount);
    if (!judged.decided || !judged.accepts)
    {
        bool emit = judged.decided && !judged.accepts;
        if (emit && calleeRes.isUnqualifiedClassCall)
        {
            if (argumentCount == 1 &&
                (IsDelegateConstructionPattern(valCtx.arguments, calleeRes.reportedName, valCtx.request.sourceCode) ||
                 HasFuncdefSymbol(calleeRes.reportedName, valCtx.ctx.request.symbolTable)))
            {
                emit = false;
            }
        }
        if (emit)
        {
            valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), "as-err-call-argument-count",
                                   calleeRes.reportedName, std::to_string(argumentCount));
        }
        return false;
    }
    return true;
}

static void CheckCall(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx)
{
    const ast::CallExpressionView callView(node, request.sourceCode);
    const TSNode callee = callView.Callee();
    const TSNode arguments = callView.ArgumentList();
    if (ts_node_is_null(callee) || ts_node_is_null(arguments) || ts_node_has_error(callee) ||
        ts_node_has_error(arguments))
    {
        return;
    }

    const CallValidationContext valCtx{node, callee, arguments, request, scope, ctx};
    const CalleeResolution calleeRes = ResolveCalleeCandidates(valCtx);
    if (!calleeRes.shouldCheck)
    {
        return;
    }

    bool sawNamedArg = false;
    if (!ValidateArgumentOrdering(arguments, sawNamedArg, ctx))
    {
        return;
    }

    const uint32_t argumentCount = CountArguments(arguments);
    if (!CheckArgumentCount(calleeRes, argumentCount, valCtx))
    {
        return;
    }

    const CallResolutionContext args = ResolveCallArguments(valCtx);
    if (std::any_of(args.argTypes.begin(), args.argTypes.end(),
                    [](const std::string& t) { return ExpressionTypeCache::IsPoisonType(t); }))
    {
        return;
    }
    const std::vector<const Symbol*> matchingArity = FilterMatchingArityCandidates(calleeRes.candidates, argumentCount);

    if (matchingArity.size() == 1 && matchingArity.front())
    {
        CheckInitializerListArgs(args, matchingArity.front()->GetFunction(), valCtx);
    }

    if (!sawNamedArg)
    {
        const LambdaCheckContext lctx{callee,
                                      arguments,
                                      calleeRes.reportedName,
                                      ctx.request.symbolTable,
                                      request.sourceCode,
                                      args.argNodes,
                                      FindLambdaPositions(args.argNodes)};
        CheckLambdaArguments(matchingArity, lctx, ctx);
    }

    CheckMalformedTernaryArgs(args.argNodes, args.argTypes, matchingArity, valCtx);
    CheckCallOverloads(matchingArity, args, calleeRes, valCtx);
    const auto site = BuildSchedulerCallSite(node, args.argNodes, request.sourceCode);
    const std::vector<std::string_view> argTypeViews(args.argTypes.begin(), args.argTypes.end());
    rules::CheckEngineSchedulerCall(
        rules::SchedulerCallRequest{site, calleeRes.reportedName, argTypeViews, matchingArity}, ctx);
}

static void CheckCallArgumentsIndexed(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    request.nodeIndex->ForEachNodeOrdered(
        std::array{gs.symCallExpression, gs.symVariableDeclaration},
        [&](TSNode node)
        {
            if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
            {
                return;
            }
            if (ts_node_symbol(node) == parser::GetGrammarSymbols().symCallExpression)
            {
                const TSPoint start = ts_node_start_point(node);
                CheckCall(node, request, FindInnermostScope(request.scopeRoot, start.row, start.column), ctx);
            }
            else
            {
                CheckVariableDirectInitialization(node, request, ctx);
            }
        });
}

void CheckCallArguments(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        CheckCallArgumentsIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.rootNode);
    AstCheckContext indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckCallArgumentsIndexed(indexedRequest, ctx);
}

} // namespace angel_lsp::analysis
