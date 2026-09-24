#include "features/code_action/CodeActionExtractMethodAnalysis.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Finds the insertion point for adding a method definition inside a class.
 * @param[in] classNode Class declaration node.
 * @return Insertion point.
 */
TSPoint FindClassBodyInsertionPoint(TSNode classNode)
{
    TSNode classBody = parser::GetChildByField(classNode, parser::fields::Body);
    if (ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classNode);
        for (uint32_t i = 0; i < cnt; ++i)
        {
            TSNode ch = ts_node_child(classNode, i);
            if (std::string_view(ts_node_type(ch)) == "class_body")
            {
                classBody = ch;
                break;
            }
        }
    }

    if (!ts_node_is_null(classBody))
    {
        uint32_t cnt = ts_node_child_count(classBody);
        for (int i = static_cast<int>(cnt) - 1; i >= 0; --i)
        {
            TSNode ch = ts_node_child(classBody, static_cast<uint32_t>(i));
            if (std::string_view(ts_node_type(ch)) == "}")
            {
                return ts_node_start_point(ch);
            }
        }
    }
    return TSPoint{0, 0};
}

/**
 * @brief Constructs the workspace edits for the new extracted method and invocation site.
 * @param[in] request Code action request.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] plan Extracted method plan.
 * @return Constructed WorkspaceEdit.
 */
lsp::WorkspaceEdit BuildExtractMethodEdits(const CodeActionRequest& request, const ExtractMethodStatements& stmts,
                                           const ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    lsp::TextEdit methodDefEdit;

    if (!ts_node_is_null(stmts.classNode))
    {
        TSPoint insertPt = FindClassBodyInsertionPoint(stmts.classNode);
        std::string methodCode = "\n    " + plan.returnType + " " + methodName + "(" + plan.paramsStr +
                                 ")\n    {\n        " + plan.extractedBody + "\n    }\n";
        methodDefEdit.range = lsp::Range{{insertPt.row, insertPt.column}, {insertPt.row, insertPt.column}};
        methodDefEdit.newText = methodCode;
    }
    else
    {
        TSPoint fnEnd = ts_node_end_point(stmts.fnNode);
        std::string methodCode = "\n\n" + plan.returnType + " " + methodName + "(" + plan.paramsStr + ")\n{\n    " +
                                 plan.extractedBody + "\n}\n";
        methodDefEdit.range = lsp::Range{{fnEnd.row + 1, 0}, {fnEnd.row + 1, 0}};
        methodDefEdit.newText = methodCode;
    }

    lsp::TextEdit callEdit;
    callEdit.range =
        lsp::Range{{stmts.firstStart.row, stmts.firstStart.column}, {stmts.lastEnd.row, stmts.lastEnd.column}};
    callEdit.newText = plan.callSiteText;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(callEdit), std::move(methodDefEdit)};
    wsEdit.changes = std::move(changes);
    return wsEdit;
}

} // namespace

void TryAddExtractMethodAction(const CodeActionRequest& request, TSNode rootNode, std::vector<lsp::CodeAction>& actions)
{
    auto stmts = FindSelectedStatements(rootNode, request);
    if (!stmts)
    {
        return;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* fnScope =
        FindScopeByLineOrRoot(rootScope.get(), ts_node_start_point(stmts->fnNode).row);
    const analysis::Scope* stmtScope =
        FindScopeByLineOrRoot(rootScope.get(), stmts->firstStart.row);

    ExtractedMethodVariables vars;
    if (fnScope)
    {
        vars.inputParams = CollectMethodInputs(*stmts, fnScope);
        auto mutated = CollectMutatedVariables(stmts->selectedStmts, request.sourceCode);
        vars.outputVars = CollectMethodOutputs(*stmts, fnScope, mutated, stmtScope);
    }

    ExtractedMethodPlan plan = DeduceExtractedMethodSignature(vars, *stmts, request);

    lsp::CodeAction action;
    action.title = "Extract Method";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);
    action.edit = BuildExtractMethodEdits(request, *stmts, plan);
    actions.push_back(std::move(action));
}

} // namespace angel_lsp::features
