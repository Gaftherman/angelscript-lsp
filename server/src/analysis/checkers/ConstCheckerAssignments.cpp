#include "analysis/checkers/ConstCheckerCommon.h"

#include <string_view>

namespace angel_lsp::analysis
{

void CheckAssignment(const ConstAssignmentFact& assignment, const ConstSiteFacts& facts, const Scope* scope,
                     DiagnosticContext& ctx)
{
    const ExpressionTypeContext constCtx(scope, ctx);

    std::string_view memberName;
    if (IsMemberOfThis(assignment.target, facts, constCtx, memberName))
    {
        ctx.EmitAtRange(assignment.target.range, "as-err-readonly-reference", memberName);
        return;
    }

    if (assignment.target.isHandle)
    {
        if (ResolveHandleConstness(assignment.target.actualTargetText, assignment.target.actualTargetBytes.startByte,
                                   facts, constCtx) == Constness::Const)
        {
            ctx.EmitAtRange(assignment.target.range, "as-err-const-assignment", assignment.target.text);
        }
        return;
    }

    if (ResolveConstness(assignment.target, facts, constCtx) != Constness::Const)
    {
        return;
    }

    ctx.EmitAtRange(assignment.target.range, "as-err-const-assignment", assignment.target.text);
}

} // namespace angel_lsp::analysis
