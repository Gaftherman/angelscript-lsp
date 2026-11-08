/**
 * @file CodeActionHandler.cpp
 * @brief Dispatcher for LSP Code Action and Code Action Resolve requests.
 */

#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{

std::optional<std::vector<lsp::CodeAction>> GetCodeActions(const CodeActionRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    std::vector<lsp::CodeAction> actions;
    TryAddRemoveUnusedVariableFixes(request, actions);
    TryAddImplementInterfaceFixes(request, actions);
    TryAddExtractVariableAction(request, actions);
    TryAddExtractMethodAction(request, actions);
    TryAddGetterSetterActions(request, actions);
    TryAddConstQualifierActions(request, actions);
    TryAddUndefinedIdentifierSuggestions(request, actions);
    TryAddHandleOnPrimitiveFix(request, actions);
    TryAddUnresolvedIncludeSuggestions(request, actions);
    TryAddAccessorPropertyKeywordFix(request, actions);
    TryAddBoolConversionFix(request, actions);
    TryAddRepeatedConversionFix(request, actions);
    TryAddGenerateFuncdefFix(request, actions);
    TryAddSortAndCleanIncludesAction(request, actions);
    TryAddDiagnosticSuppressionFixes(request, actions);

    if (actions.empty())
    {
        return std::nullopt;
    }

    return actions;
}

std::optional<lsp::CodeAction> ResolveCodeAction(const CodeActionResolveRequest& request)
{
    return request.action;
}

} // namespace angel_lsp::features
