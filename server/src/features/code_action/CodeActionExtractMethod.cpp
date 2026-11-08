#include "features/code_action/CodeActionExtractMethodAnalysis.h"
#include "features/common/FeatureSharedUtils.h"

namespace angel_lsp::features
{

/**
 * @brief Constructs the workspace edits for the new extracted method and invocation site.
 * @param[in] request Code action request.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] plan Extracted method plan.
 * @return Constructed WorkspaceEdit.
 */
static lsp::WorkspaceEdit BuildExtractMethodEdits(const CodeActionRequest& request,
                                                  const ExtractMethodStatements& stmts, const ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    lsp::TextEdit methodDefEdit;

    if (stmts.selection.inClass)
    {
        std::string methodCode = "\n    " + plan.returnType + " " + methodName + "(" + plan.paramsStr +
                                 ")\n    {\n        " + plan.extractedBody + "\n    }\n";
        methodDefEdit.range = lsp::Range{{stmts.selection.classInsertLine, stmts.selection.classInsertColumn},
                                         {stmts.selection.classInsertLine, stmts.selection.classInsertColumn}};
        methodDefEdit.newText = methodCode;
    }
    else
    {
        std::string methodCode = "\n\n" + plan.returnType + " " + methodName + "(" + plan.paramsStr + ")\n{\n    " +
                                 plan.extractedBody + "\n}\n";
        methodDefEdit.range =
            lsp::Range{{stmts.selection.functionEndLine + 1, 0}, {stmts.selection.functionEndLine + 1, 0}};
        methodDefEdit.newText = methodCode;
    }

    lsp::TextEdit callEdit;
    callEdit.range = common::ToLspRange(stmts.selection.statementsRange);
    callEdit.newText = plan.callSiteText;

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(callEdit), std::move(methodDefEdit)};
    wsEdit.changes = std::move(changes);
    return wsEdit;
}

void TryAddExtractMethodAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    auto stmts = FindSelectedStatements(request);
    if (!stmts)
    {
        return;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* fnScope = FindScopeByLineOrRoot(rootScope.get(), stmts->selection.functionStartLine);
    const analysis::Scope* stmtScope =
        FindScopeByLineOrRoot(rootScope.get(), stmts->selection.statementsRange.startLine);

    ExtractedMethodVariables vars;
    if (fnScope)
    {
        vars.inputParams = CollectMethodInputs(*stmts, fnScope);
        auto mutated = CollectMutatedVariables(*stmts, request.sourceCode);
        vars.outputVars = CollectMethodOutputs(*stmts, fnScope, mutated, stmtScope);
    }

    ExtractedMethodPlan plan = DeduceExtractedMethodSignature(vars, *stmts, request);

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-extract-method", "Extract Method");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);
    action.edit = BuildExtractMethodEdits(request, *stmts, plan);
    actions.push_back(std::move(action));
}

} // namespace angel_lsp::features
