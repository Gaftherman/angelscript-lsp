#include "analysis/checkers/ConstChecker.h"

#include "analysis/checkers/ConstCheckerCommon.h"
#include "analysis/index/ConstSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <cstddef>

namespace angel_lsp::analysis
{

void CheckConstCorrectness(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex || request.nodeIndex->Empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    const ConstSiteFacts facts = BuildConstSiteFacts(*request.nodeIndex, request.sourceCode);

    size_t assignIdx = 0;
    size_t callIdx = 0;
    while (assignIdx < facts.assignments.size() || callIdx < facts.methodCalls.size())
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }

        const bool takeAssign =
            (callIdx >= facts.methodCalls.size()) ||
            (assignIdx < facts.assignments.size() &&
             facts.assignments[assignIdx].bytes.startByte <= facts.methodCalls[callIdx].bytes.startByte);

        if (takeAssign)
        {
            const auto& assign = facts.assignments[assignIdx++];
            const Scope* scope =
                request.mediator
                    ? request.mediator->ScopeAtPoint(assign.range.startLine, assign.range.startCharacter)
                    : FindInnermostScope(request.scopeRoot, assign.range.startLine, assign.range.startCharacter);
            CheckAssignment(assign, facts, scope, ctx);
        }
        else
        {
            const auto& call = facts.methodCalls[callIdx++];
            const Scope* scope =
                request.mediator
                    ? request.mediator->ScopeAtPoint(call.range.startLine, call.range.startCharacter)
                    : FindInnermostScope(request.scopeRoot, call.range.startLine, call.range.startCharacter);
            CheckMethodCall(call, facts, scope, ctx);
        }
    }
}

} // namespace angel_lsp::analysis
