#include "analysis/checkers/CallChecker.h"

#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/InitializerListFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "utils/ScopedNameUtils.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

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

static bool CheckArgumentCount(const CalleeResolution& calleeRes, const CallCheckFactRequest& req,
                               DiagnosticContext& ctx)
{
    const uint32_t argumentCount = static_cast<uint32_t>(req.arguments.size());
    const CandidateSet judged = JudgeAgainst(calleeRes.candidates, argumentCount);
    if (!judged.decided || !judged.accepts)
    {
        bool emit = judged.decided && !judged.accepts;
        if (emit && calleeRes.isUnqualifiedClassCall)
        {
            if (argumentCount == 1 &&
                (req.site.isDelegateConstruction || HasFuncdefSymbol(calleeRes.reportedName, ctx.request.symbolTable)))
            {
                emit = false;
            }
        }
        if (emit)
        {
            const SourceRange emitRange =
                req.site.calleeNameRange != SourceRange{} ? req.site.calleeNameRange : req.site.calleeRange;
            ctx.EmitAtRange(emitRange, "as-err-call-argument-count", calleeRes.reportedName,
                            std::to_string(argumentCount));
        }
        return false;
    }
    return true;
}

static void CheckCall(const CallCheckFactRequest& req, DiagnosticContext& ctx)
{
    if (HasOverlappingError(req.site.bytes.startByte, req.site.bytes.endByte, req.request.nodeIndex))
    {
        return;
    }

    const CalleeResolution calleeRes = ResolveCalleeCandidates(req.site, req.scope, ctx);
    if (!calleeRes.shouldCheck)
    {
        return;
    }

    bool sawNamedArg = false;
    if (!ValidateArgumentOrdering(req.arguments, sawNamedArg, ctx))
    {
        return;
    }

    if (!CheckArgumentCount(calleeRes, req, ctx))
    {
        return;
    }

    const CallResolutionContext args = ResolveCallArguments(req.arguments, req.scope, ctx, req.request.nodeIndex);
    if (std::any_of(args.argTypes.begin(), args.argTypes.end(),
                    [](const std::string& t) { return ExpressionTypeCache::IsPoisonType(t); }))
    {
        return;
    }
    const uint32_t argumentCount = static_cast<uint32_t>(req.arguments.size());
    const std::vector<const Symbol*> matchingArity = FilterMatchingArityCandidates(calleeRes.candidates, argumentCount);

    if (matchingArity.size() == 1 && matchingArity.front() && req.initFacts)
    {
        CheckInitializerListArgs(req.arguments, args, matchingArity.front()->GetFunction(), *req.initFacts, req.scope,
                                 ctx);
    }

    if (!sawNamedArg)
    {
        const SourceRange calleeRange =
            req.site.calleeNameRange != SourceRange{} ? req.site.calleeNameRange : req.site.calleeRange;
        const LambdaCheckFactsContext lctx{calleeRange, calleeRes.reportedName, ctx.request.symbolTable, req.arguments,
                                           FindLambdaPositions(req.arguments)};
        CheckLambdaArguments(matchingArity, lctx, ctx);
    }

    CheckMalformedTernaryArgs(MalformedTernaryCheckRequest{req.arguments, args.argTypes, matchingArity, req.scope},
                              ctx);
    CheckCallOverloads(
        CallOverloadCheckRequest{matchingArity, args, calleeRes, req.site, req.arguments, req.initFacts, req.scope},
        ctx);

    SchedulerCallSite schedulerSite;
    schedulerSite.call.bytes = req.site.bytes;
    schedulerSite.call.startRow = req.site.range.startLine;
    schedulerSite.call.startColumn = req.site.range.startCharacter;
    schedulerSite.call.endRow = req.site.range.endLine;
    schedulerSite.call.endColumn = req.site.range.endCharacter;
    if (req.site.isMemberCall)
    {
        schedulerSite.receiverText = req.site.objectOperand.text;
    }
    schedulerSite.args.reserve(req.arguments.size());
    for (const auto& a : req.arguments)
    {
        IndexedSourceSpan s;
        s.bytes = a.bytes;
        s.startRow = a.range.startLine;
        s.startColumn = a.range.startCharacter;
        s.endRow = a.range.endLine;
        s.endColumn = a.range.endCharacter;
        schedulerSite.args.push_back(s);
    }
    const std::vector<std::string_view> argTypeViews(args.argTypes.begin(), args.argTypes.end());
    rules::CheckEngineSchedulerCall(
        rules::SchedulerCallRequest{schedulerSite, calleeRes.reportedName, argTypeViews, matchingArity}, ctx);
}

void CheckCallArguments(const CallSiteFacts& facts, const InitializerListFacts& initFacts,
                        const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    size_t callIdx = 0;
    size_t initIdx = 0;
    while (callIdx < facts.callSites.size() || initIdx < facts.directInits.size())
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }

        bool takeCall = false;
        if (callIdx < facts.callSites.size() && initIdx < facts.directInits.size())
        {
            takeCall = facts.callSites[callIdx].bytes.startByte <= facts.directInits[initIdx].bytes.startByte;
        }
        else
        {
            takeCall = (callIdx < facts.callSites.size());
        }

        if (takeCall)
        {
            const auto& site = facts.callSites[callIdx++];
            const auto arguments = std::span(facts.arguments.data() + site.argumentBegin, site.argumentCount);
            const Scope* scope = FindInnermostScope(request.scopeRoot, site.range.startLine, site.range.startCharacter);
            CheckCall(CallCheckFactRequest{site, arguments, &initFacts, request, scope}, ctx);
        }
        else
        {
            const auto& init = facts.directInits[initIdx++];
            CheckVariableDirectInitialization(init, request, facts, ctx);
        }
    }
}

void CheckCallArguments(const CallSiteFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        const InitializerListFacts initFacts = BuildInitializerListFacts(*request.nodeIndex, request.sourceCode);
        CheckCallArguments(facts, initFacts, request, ctx);
    }
    else if (request.rootNode.id != nullptr)
    {
        const NodeIndex localIndex(request.rootNode);
        AstCheckContext indexedRequest = request;
        indexedRequest.nodeIndex = &localIndex;
        const InitializerListFacts initFacts = BuildInitializerListFacts(localIndex, request.sourceCode);
        CheckCallArguments(facts, initFacts, indexedRequest, ctx);
    }
}

void CheckCallArguments(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        const CallSiteFacts facts = BuildCallSiteFacts(*request.nodeIndex, request.sourceCode);
        const InitializerListFacts initFacts = BuildInitializerListFacts(*request.nodeIndex, request.sourceCode);
        CheckCallArguments(facts, initFacts, request, ctx);
    }
    else if (request.rootNode.id != nullptr)
    {
        const NodeIndex localIndex(request.rootNode);
        AstCheckContext indexedRequest = request;
        indexedRequest.nodeIndex = &localIndex;
        const CallSiteFacts facts = BuildCallSiteFacts(localIndex, request.sourceCode);
        const InitializerListFacts initFacts = BuildInitializerListFacts(localIndex, request.sourceCode);
        CheckCallArguments(facts, initFacts, indexedRequest, ctx);
    }
}

} // namespace angel_lsp::analysis
