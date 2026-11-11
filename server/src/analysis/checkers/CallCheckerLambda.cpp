#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

std::vector<size_t> FindLambdaPositions(std::span<const CallArgumentFact> arguments)
{
    std::vector<size_t> positions;
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        if (arguments[i].kind == CallArgumentKind::Lambda)
        {
            positions.push_back(i);
        }
    }
    return positions;
}

static LambdaCandidateStatus EvaluateCandidateLambdaShape(const Symbol& candidate, const LambdaCheckFactsContext& lctx,
                                                          std::vector<std::string>& shape)
{
    const auto& fn = candidate.GetFunction();
    for (const size_t position : lctx.lambdaPositions)
    {
        if (position >= fn.parameters.size() || position >= lctx.arguments.size())
        {
            return LambdaCandidateStatus::Rejected;
        }

        const std::string_view parameterType = CleanBaseTypeView(fn.parameters[position].typeName);
        const auto funcdef = FindFuncdefSymbol(parameterType, lctx.table);
        if (!funcdef)
        {
            return LambdaCandidateStatus::UnresolvableFuncdef;
        }
        const auto& arg = lctx.arguments[position];
        if (arg.funcdefValue.hasParameterList &&
            LambdaContradictsFuncdef(arg.funcdefValue.lambdaParameters, funcdef->GetFuncdef(), lctx.table))
        {
            return LambdaCandidateStatus::Rejected;
        }
        shape.push_back(funcdef->name);
    }
    return LambdaCandidateStatus::Accepted;
}

static void ReportLambdaDiagnostics(std::span<const std::vector<std::string>> acceptedShapes,
                                    const Symbol* priorityCandidate, const LambdaCheckFactsContext& lctx,
                                    DiagnosticContext& ctx)
{
    if (acceptedShapes.empty())
    {
        ctx.EmitAtRange(lctx.calleeRange, "as-err-call-no-matching-signature", lctx.reportedName);
        return;
    }

    const bool everyShapeIdentical =
        std::all_of(acceptedShapes.begin(), acceptedShapes.end(),
                    [&](const std::vector<std::string>& shape) { return shape == acceptedShapes.front(); });
    if (acceptedShapes.size() > 1 && !everyShapeIdentical)
    {
        const std::string_view prioritySig =
            priorityCandidate != nullptr ? priorityCandidate->GetDisplaySignature() : std::string_view{};
        ctx.EmitAtRange(lctx.calleeRange, "as-err-call-ambiguous", {lctx.reportedName, prioritySig});
    }
}

void CheckLambdaArguments(std::span<const Symbol* const> candidates, const LambdaCheckFactsContext& lctx,
                          DiagnosticContext& ctx)
{
    if (lctx.lambdaPositions.empty() || candidates.empty())
    {
        return;
    }

    std::vector<std::vector<std::string>> acceptedShapes;
    const Symbol* firstAccepted = nullptr;
    for (const auto* candidate : candidates)
    {
        if (!candidate)
        {
            continue;
        }
        std::vector<std::string> shape;
        const auto status = EvaluateCandidateLambdaShape(*candidate, lctx, shape);
        if (status == LambdaCandidateStatus::UnresolvableFuncdef)
        {
            return;
        }
        if (status == LambdaCandidateStatus::Accepted)
        {
            if (!firstAccepted)
            {
                firstAccepted = candidate;
            }
            acceptedShapes.push_back(std::move(shape));
        }
    }

    ReportLambdaDiagnostics(acceptedShapes, firstAccepted, lctx, ctx);
}

} // namespace angel_lsp::analysis
