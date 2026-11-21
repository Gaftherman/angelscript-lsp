#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

static size_t ResolveParameterIndexForArgument(const FunctionSignature& sig, size_t argIdx, std::string_view argName,
                                               const ankerl::unordered_dense::set<size_t>& matchedParams)
{
    if (!argName.empty())
    {
        for (size_t p = 0; p < sig.parameters.size(); ++p)
        {
            if (sig.parameters[p].name == argName)
            {
                return matchedParams.contains(p) ? size_t(-1) : p;
            }
        }
        return size_t(-1);
    }

    if (argIdx >= sig.parameters.size())
    {
        return InspectFunctionArity(sig).isVariadic ? argIdx : size_t(-1);
    }
    return matchedParams.contains(argIdx) ? size_t(-1) : argIdx;
}

static bool CheckUnmatchedDefaultParameters(const FunctionSignature& sig,
                                            const ankerl::unordered_dense::set<size_t>& matchedParams)
{
    for (size_t p = 0; p < sig.parameters.size(); ++p)
    {
        if (!matchedParams.contains(p) && sig.parameters[p].defaultValue.empty())
        {
            return false;
        }
    }
    return true;
}

static std::optional<ankerl::unordered_dense::set<size_t>>
CheckCandidateNamedArgs(const FunctionSignature& sig, std::span<const std::string> argNames,
                        std::span<const std::string> argTypes, const SymbolTable& symbolTable)
{
    ankerl::unordered_dense::set<size_t> matchedParams;
    for (size_t i = 0; i < argNames.size(); ++i)
    {
        size_t paramIdx = ResolveParameterIndexForArgument(sig, i, argNames[i], matchedParams);
        if (paramIdx == size_t(-1))
        {
            return std::nullopt;
        }
        if (paramIdx < sig.parameters.size())
        {
            matchedParams.insert(paramIdx);
            if (!argTypes[i].empty())
            {
                const auto conv = EvaluateArgumentConversion(argTypes[i], sig.parameters[paramIdx], symbolTable, true);
                if (!conv.IsViable())
                {
                    return std::nullopt;
                }
            }
        }
    }

    if (!CheckUnmatchedDefaultParameters(sig, matchedParams))
    {
        return std::nullopt;
    }
    return matchedParams;
}

void PublishCallBinding(DiagnosticContext& ctx, uint32_t useByte, const SourceRange& use, const Symbol& declaration)
{
    if (!ctx.request.mutableScopeRoot || declaration.type != SymbolType::Function || declaration.isSynthesized ||
        declaration.fileUri.empty() || declaration.selectionRange == SourceRange{})
    {
        return;
    }
    auto& bindings = ctx.callBindings;
    if (!bindings)
    {
        const auto& request = ctx.request;
        bindings = std::make_shared<CallBindings>(request.callBindingRevision,
                                                  request.moduleContext ? std::string_view(request.moduleContext->name)
                                                                        : std::string_view{});
    }
    bindings->Add(useByte, use, declaration);
}

static void RecordBoundCall(const Symbol& declaration, const CallSiteFact& site, DiagnosticContext& ctx)
{
    const std::string_view calleeName = CallSiteFacts::SliceText(ctx.request.sourceCode, site.calleeNameBytes);
    if (calleeName.empty() || calleeName != declaration.name)
    {
        return;
    }
    PublishCallBinding(ctx, site.calleeNameBytes.startByte, site.calleeNameRange, declaration);
}

static void HandleNamedCallOverloads(const CallOverloadCheckRequest& req, DiagnosticContext& ctx)
{
    const Symbol* matched = nullptr;
    ankerl::unordered_dense::set<size_t> matchedParams;
    for (const Symbol* cand : req.matchingArity)
    {
        if (!cand || !std::holds_alternative<FunctionSignature>(cand->signature))
        {
            continue;
        }
        if (auto candParams = CheckCandidateNamedArgs(cand->GetFunction(), req.args.argNames, req.args.argTypes,
                                                      ctx.request.symbolTable))
        {
            if (matched)
            {
                return;
            }
            matched = cand;
            matchedParams = std::move(*candParams);
        }
    }
    if (!req.args.allArgsResolved)
    {
        return;
    }
    if (matched)
    {
        RecordBoundCall(*matched, req.site, ctx);
        CheckOmittedNamedDefaultArguments(*matched, matchedParams, req.site, ctx);
    }
    else
    {
        ctx.EmitAtRange(req.site.calleeRange, "as-err-call-no-matching-signature", req.calleeRes.reportedName);
    }
}

static void ValidateResolvedCall(const OverloadMatchResult& match, const CallOverloadCheckRequest& req,
                                 DiagnosticContext& ctx)
{
    const auto& selected = *match.bestCandidate;
    if (!match.isAmbiguous && req.args.allArgsResolved)
    {
        RecordBoundCall(selected, req.site, ctx);
    }
    CheckOmittedDefaultArguments(selected, req.args.argTypes.size(), req.site, ctx);
    ValidateOutArguments(selected, req.arguments, req.args.argBindings, ctx);
    if (req.matchingArity.size() > 1 && req.initFacts)
    {
        CheckInitializerListArgs(req.arguments, req.args, selected.GetFunction(), *req.initFacts, req.scope, ctx);
    }
}

void CheckCallOverloads(const CallOverloadCheckRequest& request, DiagnosticContext& ctx)
{
    if (request.matchingArity.empty())
    {
        return;
    }
    if (request.args.argTypes.empty() && !request.calleeRes.candidatesAreFreeFunctions)
    {
        if (request.matchingArity.size() == 1 && request.matchingArity.front())
        {
            RecordBoundCall(*request.matchingArity.front(), request.site, ctx);
            CheckOmittedDefaultArguments(*request.matchingArity.front(), 0, request.site, ctx);
        }
        return;
    }

    const bool hasNamedArgs = std::any_of(request.args.argNames.begin(), request.args.argNames.end(),
                                          [](std::string_view n) { return !n.empty(); });
    if (hasNamedArgs)
    {
        HandleNamedCallOverloads(request, ctx);
        return;
    }

    OverloadMatchResult match = ResolveBestOverload(request.matchingArity, request.args.argTypes,
                                                    ctx.request.symbolTable, request.args.argBindings);
    if (match.isAmbiguous && request.args.allArgsResolved)
    {
        const std::string_view prioritySig =
            match.priorityCandidate != nullptr ? match.priorityCandidate->GetDisplaySignature() : std::string_view{};
        const SourceRange emitRange =
            request.site.calleeNameRange != SourceRange{} ? request.site.calleeNameRange : request.site.calleeRange;
        ctx.EmitAtRange(emitRange, "as-err-call-ambiguous", {request.calleeRes.reportedName, prioritySig},
                        DiagnosticSeverity::Error);
    }
    else if (match.viableCandidates.empty() || match.bestCandidate == nullptr)
    {
        ReportOverloadResolutionFailure(
            OverloadFailureReportRequest{request.matchingArity, request.arguments, request.args, request.site}, ctx);
    }
    else
    {
        ValidateResolvedCall(match, request, ctx);
    }
}

} // namespace angel_lsp::analysis
