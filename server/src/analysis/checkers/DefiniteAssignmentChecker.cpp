#include "analysis/checkers/DefiniteAssignmentChecker.h"

#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/DefiniteAssignmentFacts.h"
#include "analysis/index/NodeIndex.h"
#include "utils/Utils.h"

namespace angel_lsp::analysis
{

DefiniteAssignmentVisitor::DefiniteAssignmentVisitor(const AstCheckContext& request, DiagnosticContext& ctx,
                                                     const DefiniteAssignmentFacts& facts)
    : m_request(request), m_ctx(ctx), m_facts(facts)
{
}

void DefiniteAssignmentVisitor::AnalyzeFunction(const DefiniteAssignmentFunctionFact& fn)
{
    m_trackedLocals.clear();
    m_reportedReads.clear();

    if (fn.rootRecordIndex >= m_facts.records.size())
    {
        return;
    }

    DefiniteAssignmentState state;
    Schedule(fn.rootRecordIndex, state);
    while (!m_work.empty())
    {
        if (Step(m_work.back()))
        {
            m_work.pop_back();
        }
    }
}

void CheckDefiniteAssignment(const DefiniteAssignmentFacts& facts, const AstCheckContext& request,
                             DiagnosticContext& ctx)
{
    if (facts.functions.empty() || request.sourceCode.empty() ||
        utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    DefiniteAssignmentVisitor visitor(request, ctx, facts);
    for (const auto& fn : facts.functions)
    {
        visitor.AnalyzeFunction(fn);
    }
}

void CheckDefiniteAssignment(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        const DefiniteAssignmentFacts facts = BuildDefiniteAssignmentFacts(*request.nodeIndex, request.sourceCode);
        CheckDefiniteAssignment(facts, request, ctx);
    }
}

} // namespace angel_lsp::analysis
