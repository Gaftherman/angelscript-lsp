#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

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

static bool CheckCandidateNamedArgs(const FunctionSignature& sig, std::span<const std::string> argNames,
                                    std::span<const std::string> argTypes, const SymbolTable& symbolTable)
{
    ankerl::unordered_dense::set<size_t> matchedParams;
    for (size_t i = 0; i < argNames.size(); ++i)
    {
        size_t paramIdx = ResolveParameterIndexForArgument(sig, i, argNames[i], matchedParams);
        if (paramIdx == size_t(-1))
        {
            return false;
        }
        if (paramIdx < sig.parameters.size())
        {
            matchedParams.insert(paramIdx);
            if (!argTypes[i].empty())
            {
                const auto conv = EvaluateArgumentConversion(argTypes[i], sig.parameters[paramIdx], symbolTable, true);
                if (!conv.IsViable())
                {
                    return false;
                }
            }
        }
    }

    return CheckUnmatchedDefaultParameters(sig, matchedParams);
}

static void RecordBoundCall(const Symbol& declaration, const CallSiteFact& site, DiagnosticContext& ctx)
{
    if (!ctx.request.mutableScopeRoot || declaration.type != SymbolType::Function || declaration.isSynthesized ||
        declaration.fileUri.empty() || declaration.selectionRange == SourceRange{})
    {
        return;
    }
    if (site.calleeName.empty() || site.calleeName != declaration.name)
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
    bindings->Add(site.calleeNameBytes.startByte, site.calleeNameRange, declaration);
}

static void HandleNamedCallOverloads(const CallOverloadCheckRequest& req, DiagnosticContext& ctx)
{
    const Symbol* matched = nullptr;
    for (const Symbol* cand : req.matchingArity)
    {
        if (cand && std::holds_alternative<FunctionSignature>(cand->signature) &&
            CheckCandidateNamedArgs(cand->GetFunction(), req.args.argNames, req.args.argTypes, ctx.request.symbolTable))
        {
            if (matched)
            {
                return;
            }
            matched = cand;
        }
    }
    if (!req.args.allArgsResolved)
    {
        return;
    }
    if (matched)
    {
        RecordBoundCall(*matched, req.site, ctx);
    }
    else
    {
        ctx.EmitAtRange(req.site.calleeRange, "as-err-call-no-matching-signature", req.calleeRes.reportedName);
    }
}

static bool IsDefaultArgumentTypeMismatch(const ParameterInformation& param, const SymbolTable& table)
{
    if (param.defaultValue.empty())
    {
        return false;
    }
    std::string_view dv = param.defaultValue;
    TrimTypeWhitespace(dv);
    if (dv.empty())
    {
        return false;
    }
    if (dv == "void")
    {
        return param.modifier != ParameterModifier::Out;
    }

    const std::string_view rawBase =
        CleanBaseTypeView(param.baseTypeName.empty() ? param.typeName : param.baseTypeName);
    const std::string paramBase = UnwrapTypedef(rawBase, table);
    if (paramBase.empty() || paramBase == "auto" || paramBase == "?" || paramBase == "any" || param.isArray ||
        !param.templateName.empty())
    {
        return false;
    }

    const bool isStringLit = dv.size() >= 2 && dv.front() == '"' && dv.back() == '"';
    const bool isBoolLit = (dv == "true" || dv == "false");
    const bool isNullLit = (dv == "null");
    const bool isEnum = ResolvesToEnum(paramBase, table);

    return (isStringLit && (IsNumericPrimitive(paramBase) || paramBase == "bool" || isEnum)) ||
           (isBoolLit && (IsNumericPrimitive(paramBase) || isEnum || param.isHandle)) ||
           (isNullLit && !param.isHandle && (IsCorePrimitive(paramBase) || isEnum));
}

static void CheckOmittedDefaultArguments(const Symbol& candidate, size_t providedPositionalCount,
                                         const CallSiteFact& site, DiagnosticContext& ctx)
{
    if (candidate.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(candidate.signature) ||
        IsFromPredefinedStub(candidate, ctx))
    {
        return;
    }
    const auto& sig = candidate.GetFunction();
    for (size_t p = providedPositionalCount; p < sig.parameters.size(); ++p)
    {
        if (IsDefaultArgumentTypeMismatch(sig.parameters[p], ctx.request.symbolTable))
        {
            ctx.EmitAtRange(site.range, diagnostics::codes::DefaultArgTypeMismatch);
            return;
        }
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
    ValidateOutArguments(selected, req.arguments, req.scope, ctx);
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
                                                    ctx.request.symbolTable, request.args.argIsLValue);
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
