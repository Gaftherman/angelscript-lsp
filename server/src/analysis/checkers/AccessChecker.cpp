#include "analysis/checkers/AccessChecker.h"

#include "analysis/checkers/AccessCheckerCommon.h"
#include "analysis/index/MemberAccessFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <cstddef>

namespace angel_lsp::analysis
{

void CheckMemberAccess(const MemberAccessFacts& facts, const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    size_t memIdx = 0;
    size_t idIdx = 0;
    const size_t memCount = facts.memberExpressions.size();
    const size_t idCount = facts.identifiers.size();

    while (memIdx < memCount || idIdx < idCount)
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }

        const bool takeMember =
            (idIdx >= idCount) || (memIdx < memCount && facts.memberExpressions[memIdx].bytes.startByte <=
                                                            facts.identifiers[idIdx].bytes.startByte);

        if (takeMember)
        {
            const auto& fact = facts.memberExpressions[memIdx++];
            const Scope* scope =
                request.mediator
                    ? request.mediator->ScopeAtPoint(fact.range.startLine, fact.range.startCharacter)
                    : FindInnermostScope(request.scopeRoot, fact.range.startLine, fact.range.startCharacter);
            CheckMemberExpression(fact, scope, ctx);
        }
        else
        {
            const auto& fact = facts.identifiers[idIdx++];
            const Scope* scope =
                request.mediator
                    ? request.mediator->ScopeAtPoint(fact.range.startLine, fact.range.startCharacter)
                    : FindInnermostScope(request.scopeRoot, fact.range.startLine, fact.range.startCharacter);
            CheckIdentifierFact(fact, scope, ctx);
        }
    }
}

void CheckMemberAccess(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    const MemberAccessFacts facts = BuildMemberAccessFacts(*request.nodeIndex, request.sourceCode);
    CheckMemberAccess(facts, request, ctx);
}

} // namespace angel_lsp::analysis
