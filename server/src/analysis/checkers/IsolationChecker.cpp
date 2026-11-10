#include "analysis/checkers/IsolationChecker.h"

#include "analysis/checkers/IsolationCheckerCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

IsolationVisitor::IsolationVisitor(const AstCheckContext& req, DiagnosticContext& dctx) : request(req), ctx(dctx)
{
}

void IsolationVisitor::PushChildren(TSNode node, const std::string& className, bool isClassShared, bool inSharedContext,
                                    std::vector<IsoItem>& worklist)
{
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = count; i > 0; --i)
    {
        TSNode child = ts_node_child(node, i - 1);
        if (!ts_node_is_null(child))
        {
            worklist.push_back({child, className, isClassShared, inSharedContext});
        }
    }
}

void IsolationVisitor::Visit(TSNode rootNode)
{
    if (ts_node_is_null(rootNode))
    {
        return;
    }

    std::vector<IsoItem> worklist;
    worklist.push_back({rootNode, "", false, false});

    const auto& gs = parser::GetGrammarSymbols();

    while (!worklist.empty())
    {
        IsoItem item = std::move(worklist.back());
        worklist.pop_back();

        TSNode node = item.node;
        const TSSymbol nodeSym = ts_node_symbol(node);

        currentClassName = item.className;
        isCurrentClassShared = item.isClassShared;

        if (nodeSym == gs.symClassDeclaration)
        {
            TSNode nameNode = parser::GetChildByField(node, gs.fieldName);
            std::string className(parser::NodeText(nameNode, request.sourceCode));
            bool classShared = IsClassShared(node, className);
            PushChildren(node, className, classShared, classShared, worklist);
            continue;
        }

        if (nodeSym == gs.symFuncDeclaration)
        {
            bool funcShared = IsFunctionShared(node, item.inSharedContext);
            PushChildren(node, item.className, item.isClassShared, funcShared, worklist);
            continue;
        }

        if (item.inSharedContext)
        {
            if (nodeSym == gs.symDatatype)
            {
                CheckSharedDatatype(node);
            }
            else if (nodeSym == gs.symCallExpression)
            {
                CheckSharedCallExpression(node);
            }
            else if (nodeSym == gs.symIdentifier)
            {
                CheckSharedIdentifier(node);
            }
        }

        PushChildren(node, item.className, item.isClassShared, item.inSharedContext, worklist);
    }
}

void CheckSharedIsolation(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (request.nodeIndex && request.nodeIndex->Nodes(gs.tokShared).empty())
    {
        return;
    }

    CheckSharedEntityEligibility(request, ctx);

    IsolationVisitor visitor(request, ctx);
    visitor.Visit(request.rootNode);
}

} // namespace angel_lsp::analysis
