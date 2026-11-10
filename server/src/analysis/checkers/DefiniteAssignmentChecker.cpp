#include "analysis/checkers/DefiniteAssignmentChecker.h"

#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <initializer_list>

namespace angel_lsp::analysis
{

DefiniteAssignmentVisitor::DefiniteAssignmentVisitor(const AstCheckContext& request, DiagnosticContext& ctx)
    : m_request(request), m_ctx(ctx), m_index(*request.nodeIndex)
{
}

void DefiniteAssignmentVisitor::AnalyzeFunction(SyntaxNodeId callable)
{
    m_trackedLocals.clear();
    m_reportedReads.clear();

    const auto& gs = parser::GetGrammarSymbols();
    const IndexedSyntaxNode* entry = m_index.SyntaxNode(callable);
    const IndexedSyntaxNode* body = FindField(entry, gs.fieldBody);
    if (!body)
    {
        body = FindOperand(entry, [&](const IndexedSyntaxNode& item) { return item.symbol == gs.symStatementBlock; });
    }

    DefiniteAssignmentState state;
    Schedule(body, state);
    while (!m_work.empty())
    {
        if (Step(m_work.back()))
        {
            m_work.pop_back();
        }
    }
}

void CheckDefiniteAssignment(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (!request.nodeIndex || request.sourceCode.empty() ||
        utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    DefiniteAssignmentVisitor visitor(request, ctx);
    for (const bool lambdas : {false, true})
    {
        for (const auto callable : request.nodeIndex->AssignmentCallables(lambdas))
        {
            visitor.AnalyzeFunction(callable);
        }
    }
}

} // namespace angel_lsp::analysis
