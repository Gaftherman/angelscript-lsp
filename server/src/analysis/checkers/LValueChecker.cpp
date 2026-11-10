#include "analysis/checkers/LValueChecker.h"

#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/LValueSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

namespace angel_lsp::analysis
{

/**
 * @brief Traverses indexed assignment expressions and evaluates left-hand target l-values.
 * @param[in] request Contextual request bundle containing pre-indexed syntax tree and scope root.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckLValues(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex || request.nodeIndex->Empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    const LValueSiteFacts facts = BuildLValueSiteFacts(*request.nodeIndex, request.sourceCode);

    for (const auto& assignment : facts.assignments)
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }

        const Scope* scope =
            FindInnermostScope(request.scopeRoot, assignment.range.startLine, assignment.range.startCharacter);

        if (assignment.targetIsDirectCall || assignment.targetIsUnwrappedCall)
        {
            if (const auto* callSite = facts.FindCallSite(assignment.callBytes))
            {
                CheckCallLValue(*callSite, request, scope, ctx);
            }
            continue;
        }

        if (assignment.targetIsDirectTernary)
        {
            continue;
        }

        if (!LValueNodeValidator::IsAssignableLValueNode(assignment.targetCandidate, facts, scope, ctx))
        {
            const ExpressionTypeContext exprCtx(scope, ctx);
            if (const auto dt = IsBareDataType(assignment.targetCandidate.unwrappedBytes, exprCtx))
            {
                ctx.EmitAtRange(assignment.targetCandidate.range, diagnostics::codes::ExpressionIsDataType, *dt);
            }
            else
            {
                ctx.EmitAtRange(assignment.targetCandidate.range, "as-err-not-lvalue");
            }
        }
    }
}

} // namespace angel_lsp::analysis
