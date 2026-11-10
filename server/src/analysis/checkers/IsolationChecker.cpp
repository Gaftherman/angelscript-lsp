#include "analysis/checkers/IsolationChecker.h"

#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/IsolationSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "utils/StopFlag.h"

#include <cstddef>
#include <cstdint>

namespace angel_lsp::analysis
{

void CheckSharedIsolation(const IsolationSiteFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    if (facts.sharedModifiers.empty())
    {
        return;
    }

    CheckSharedEntityEligibility(facts, ctx);

    size_t typeIdx = 0;
    size_t callIdx = 0;
    size_t idIdx = 0;
    const size_t typeCount = facts.types.size();
    const size_t callCount = facts.calls.size();
    const size_t idCount = facts.identifiers.size();

    while (typeIdx < typeCount || callIdx < callCount || idIdx < idCount)
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }

        const uint32_t typeByte = (typeIdx < typeCount) ? facts.types[typeIdx].bytes.startByte : UINT32_MAX;
        const uint32_t callByte = (callIdx < callCount) ? facts.calls[callIdx].bytes.startByte : UINT32_MAX;
        const uint32_t idByte = (idIdx < idCount) ? facts.identifiers[idIdx].bytes.startByte : UINT32_MAX;

        if (typeByte <= callByte && typeByte <= idByte)
        {
            CheckSharedDatatype(facts.types[typeIdx++], ctx);
        }
        else if (callByte <= idByte)
        {
            CheckSharedCallExpression(facts.calls[callIdx++], ctx);
        }
        else
        {
            CheckSharedIdentifier(facts.identifiers[idIdx++], request.scopeRoot, ctx);
        }
    }
}

void CheckSharedIsolation(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }

    const IsolationSiteFacts facts = BuildIsolationSiteFacts(*request.nodeIndex, request.sourceCode);
    CheckSharedIsolation(facts, request, ctx);
}

} // namespace angel_lsp::analysis
