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

    TSNode rootNode = ts_tree_root_node(request.tree);
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    std::vector<lsp::CodeAction> actions;
    TryAddRemoveUnusedVariableFixes(request, rootNode, actions);
    TryAddImplementInterfaceFixes(request, rootNode, actions);
    TryAddExtractVariableAction(request, rootNode, actions);
    TryAddExtractMethodAction(request, rootNode, actions);
    TryAddGetterSetterActions(request, rootNode, actions);
    TryAddConstQualifierActions(request, rootNode, actions);
    TryAddUndefinedIdentifierSuggestions(request, rootNode, actions);
    TryAddHandleOnPrimitiveFix(request, rootNode, actions);
    TryAddUnresolvedIncludeSuggestions(request, actions);
    TryAddAccessorPropertyKeywordFix(request, rootNode, actions);
    TryAddBoolConversionFix(request, rootNode, actions);
    TryAddGenerateFuncdefFix(request, rootNode, actions);
    TryAddSortAndCleanIncludesAction(request, actions);

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
