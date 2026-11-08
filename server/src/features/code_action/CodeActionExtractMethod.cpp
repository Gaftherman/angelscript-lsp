#include "features/code_action/CodeActionExtractMethodAnalysis.h"

#include <string>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Constructs text edits for the new extracted method and invocation site.
 * @param[in] stmts Extracted statement metadata.
 * @param[in] plan Extracted method plan.
 * @return Constructed vector of code action text edits.
 */
static std::vector<analysis::code_action::CodeActionTextEdit>
BuildExtractMethodTextEdits(const ExtractMethodStatements& stmts, const ExtractedMethodPlan& plan)
{
    const std::string methodName = "NewMethod";
    analysis::code_action::CodeActionTextEdit methodDefEdit;

    if (stmts.selection.inClass)
    {
        std::string methodCode = "\n    " + plan.returnType + " " + methodName + "(" + plan.paramsStr +
                                 ")\n    {\n        " + plan.extractedBody + "\n    }\n";
        methodDefEdit.range = analysis::SourceRange{stmts.selection.classInsertLine, stmts.selection.classInsertColumn,
                                                    stmts.selection.classInsertLine, stmts.selection.classInsertColumn};
        methodDefEdit.newText = std::move(methodCode);
    }
    else
    {
        std::string methodCode = "\n\n" + plan.returnType + " " + methodName + "(" + plan.paramsStr + ")\n{\n    " +
                                 plan.extractedBody + "\n}\n";
        methodDefEdit.range =
            analysis::SourceRange{stmts.selection.functionEndLine + 1, 0, stmts.selection.functionEndLine + 1, 0};
        methodDefEdit.newText = std::move(methodCode);
    }

    analysis::code_action::CodeActionTextEdit callEdit;
    callEdit.range = stmts.selection.statementsRange;
    callEdit.newText = plan.callSiteText;

    return {std::move(callEdit), std::move(methodDefEdit)};
}

/**
 * @brief Generates a refactoring entry to extract statements into a method.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddExtractMethodAction(const analysis::code_action::CodeActionQuery& query,
                               std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    auto stmts = FindSelectedStatements(query);
    if (!stmts)
    {
        return;
    }

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    const analysis::Scope* fnScope = FindScopeByLineOrRoot(rootScope.get(), stmts->selection.functionStartLine);
    const analysis::Scope* stmtScope =
        FindScopeByLineOrRoot(rootScope.get(), stmts->selection.statementsRange.startLine);

    ExtractedMethodVariables vars;
    if (fnScope)
    {
        vars.inputParams = CollectMethodInputs(*stmts, fnScope);
        auto mutated = CollectMutatedVariables(*stmts, query.sourceCode);
        vars.outputVars = CollectMethodOutputs(*stmts, fnScope, mutated, stmtScope);
    }

    ExtractedMethodPlan plan = DeduceExtractedMethodSignature(vars, *stmts, query);

    analysis::code_action::CodeActionEntry entry;
    entry.title = i18n::FormatMessage(query.i18n, "action-extract-method", "Extract Method");
    entry.kind = analysis::code_action::CodeActionEntryKind::RefactorExtract;
    entry.edits = BuildExtractMethodTextEdits(*stmts, plan);
    entries.push_back(std::move(entry));
}

} // namespace angel_lsp::features
