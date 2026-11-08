#include "analysis/code_action/CodeActionEngine.h"

#include "analysis/code_action/CodeActionInternal.h"

namespace angel_lsp::analysis::code_action
{

std::vector<CodeActionEntry> CollectCodeActionEntries(const CodeActionQuery& query)
{
    std::vector<CodeActionEntry> entries;
    TryAddRemoveUnusedVariableFixes(query, entries);
    TryAddImplementInterfaceFixes(query, entries);
    TryAddExtractVariableAction(query, entries);
    TryAddExtractMethodAction(query, entries);
    TryAddGetterSetterActions(query, entries);
    TryAddConstQualifierActions(query, entries);
    TryAddUndefinedIdentifierSuggestions(query, entries);
    TryAddHandleOnPrimitiveFix(query, entries);
    TryAddUnresolvedIncludeSuggestions(query, entries);
    TryAddAccessorPropertyKeywordFix(query, entries);
    TryAddBoolConversionFix(query, entries);
    TryAddRepeatedConversionFix(query, entries);
    TryAddGenerateFuncdefFix(query, entries);
    TryAddSortAndCleanIncludesAction(query, entries);
    TryAddDiagnosticSuppressionFixes(query, entries);
    return entries;
}

} // namespace angel_lsp::analysis::code_action
