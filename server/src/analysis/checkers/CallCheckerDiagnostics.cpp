#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsTernaryBranchCompatible(std::string_view branchType, std::string_view expected)
{
    if (branchType.empty())
    {
        return true;
    }
    if (branchType == expected || IsSameType(branchType, expected))
    {
        return true;
    }
    if (parser::primitives::IsNullType(branchType) && IsHandleTypeView(expected))
    {
        return true;
    }
    return false;
}

static std::string_view ResolveTernaryMismatchType(std::string_view t1, std::string_view t2, std::string_view expected)
{
    if (!t1.empty() && !IsTernaryBranchCompatible(t1, expected))
    {
        return t1;
    }
    if (!t2.empty() && !IsTernaryBranchCompatible(t2, expected))
    {
        return t2;
    }
    return !t1.empty() ? t1 : (!t2.empty() ? t2 : "unknown");
}

void CheckMalformedTernaryArgs(const MalformedTernaryCheckRequest& req, DiagnosticContext& ctx)
{
    if (req.candidates.empty() || !req.candidates[0])
    {
        return;
    }
    const auto& fn = req.candidates[0]->GetFunction();
    const size_t limit = std::min({req.arguments.size(), req.argTypes.size(), fn.parameters.size()});

    for (size_t i = 0; i < limit; ++i)
    {
        const auto& arg = req.arguments[i];
        if (arg.kind != CallArgumentKind::Ternary || !req.argTypes[i].empty())
        {
            continue;
        }
        if (IsWildcardParameter(fn.parameters[i]))
        {
            continue;
        }

        const std::string_view expected = fn.parameters[i].typeName;
        const auto ternary = req.facts.TernaryOf(arg);
        std::string t1 = ResolveExpressionType(ternary.consequenceBytes, ExpressionTypeContext(req.scope, ctx));
        std::string t2 = ResolveExpressionType(ternary.alternativeBytes, ExpressionTypeContext(req.scope, ctx));

        if (IsTernaryBranchCompatible(t1, expected) && IsTernaryBranchCompatible(t2, expected))
        {
            continue;
        }

        const std::string_view badType = ResolveTernaryMismatchType(t1, t2, expected);
        ctx.EmitAtRange(arg.range, "as-err-no-implicit-conversion", badType, expected);
    }
}

static size_t FindCandidateBadArgument(const Symbol& candidate, std::span<const std::string> argTypes,
                                       const SymbolTable& table)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < argTypes.size() && i < fn.parameters.size(); ++i)
    {
        if (!EvaluateArgumentConversion(argTypes[i], fn.parameters[i], table).IsViable())
        {
            return i;
        }
    }
    return argTypes.size();
}

static std::optional<size_t> FindAgreedBlamedArgument(std::span<const Symbol* const> candidates,
                                                      std::span<const std::string> argTypes, const SymbolTable& table)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }

    size_t blamed = argTypes.size();
    for (const auto* candidate : candidates)
    {
        if (!candidate)
        {
            continue;
        }
        const size_t firstBad = FindCandidateBadArgument(*candidate, argTypes, table);
        if (firstBad == argTypes.size())
        {
            return std::nullopt;
        }
        if (blamed == argTypes.size())
        {
            blamed = firstBad;
        }
        else if (blamed != firstBad)
        {
            return std::nullopt;
        }
    }
    return blamed < argTypes.size() ? std::optional<size_t>(blamed) : std::nullopt;
}

static std::string FormatCallDescriptionWithNull(std::string_view calleeName, std::span<const std::string> argTypes)
{
    std::string callDesc(calleeName);
    callDesc += "(";
    for (size_t i = 0; i < argTypes.size(); ++i)
    {
        if (i > 0)
        {
            callDesc += ", ";
        }
        callDesc += (parser::primitives::IsNullType(argTypes[i]) ? "<null handle>" : argTypes[i]);
    }
    callDesc += ")";
    return callDesc;
}

void ReportOverloadResolutionFailure(const OverloadFailureReportRequest& req, DiagnosticContext& ctx)
{
    const std::string_view calleeName = CallSiteFacts::SliceText(ctx.request.sourceCode, req.site.calleeNameBytes);
    const auto blamedArg = FindAgreedBlamedArgument(req.candidates, req.args.argTypes, ctx.request.symbolTable);
    if (blamedArg && *blamedArg < req.arguments.size())
    {
        if (parser::primitives::IsNullType(req.args.argTypes[*blamedArg]))
        {
            const SourceRange emitRange =
                req.site.calleeNameRange != SourceRange{} ? req.site.calleeNameRange : req.site.calleeRange;
            ctx.EmitAtRange(emitRange, "as-err-call-no-matching-signature",
                            FormatCallDescriptionWithNull(calleeName, req.args.argTypes));
            return;
        }

        const auto& fn = req.candidates[0]->GetFunction();
        const std::string_view expected = *blamedArg < fn.parameters.size()
                                              ? std::string_view(fn.parameters[*blamedArg].typeName)
                                              : std::string_view();
        ctx.EmitAtRange(req.arguments[*blamedArg].range, "as-err-no-implicit-conversion", req.args.argTypes[*blamedArg],
                        expected);
        return;
    }

    if (req.args.allArgsResolved)
    {
        const bool hasNull = std::any_of(req.args.argTypes.begin(), req.args.argTypes.end(),
                                         [](std::string_view t) { return parser::primitives::IsNullType(t); });
        const SourceRange emitRange =
            req.site.calleeNameRange != SourceRange{} ? req.site.calleeNameRange : req.site.calleeRange;
        ctx.EmitAtRange(emitRange, "as-err-call-no-matching-signature",
                        hasNull ? FormatCallDescriptionWithNull(calleeName, req.args.argTypes) : calleeName);
    }
}

} // namespace angel_lsp::analysis
