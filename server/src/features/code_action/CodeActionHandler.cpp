/**
 * @file CodeActionHandler.cpp
 * @brief Dispatcher for LSP Code Action and Code Action Resolve requests.
 */

#include "features/code_action/CodeActionInternal.h"
#include "features/common/FeatureSharedUtils.h"

#include <variant>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Copies an LSP protocol range into the protocol-free analysis range.
 * @param[in] range LSP range from the request or a diagnostic.
 * @return Equivalent analysis source range.
 */
static analysis::SourceRange FromLspRange(const lsp::Range& range) noexcept
{
    return analysis::SourceRange{range.start.line, range.start.character, range.end.line, range.end.character};
}

/**
 * @brief Constructs lightweight diagnostic views from LSP context diagnostics.
 * @param[in] context LSP CodeAction context containing request diagnostics.
 * @return Vector of diagnostic views referencing the context diagnostics.
 */
static std::vector<analysis::code_action::CodeActionDiagnosticView>
BuildDiagnosticViews(const lsp::CodeActionContext& context)
{
    std::vector<analysis::code_action::CodeActionDiagnosticView> views;
    views.reserve(context.diagnostics.size());
    for (const auto& diag : context.diagnostics)
    {
        std::string_view code;
        if (diag.code.has_value() && std::holds_alternative<lsp::String>(diag.code.value()))
        {
            code = std::get<lsp::String>(diag.code.value());
        }

        std::string_view message;
        bool messageIsPlainText = true;
        if (std::holds_alternative<lsp::String>(diag.message))
        {
            message = std::get<lsp::String>(diag.message);
            messageIsPlainText = true;
        }
        else if (std::holds_alternative<lsp::MarkupContent>(diag.message))
        {
            message = std::get<lsp::MarkupContent>(diag.message).value;
            messageIsPlainText = false;
        }

        views.push_back(analysis::code_action::CodeActionDiagnosticView{
            .range = FromLspRange(diag.range),
            .code = code,
            .message = message,
            .messageIsPlainText = messageIsPlainText,
        });
    }
    return views;
}

/**
 * @brief Maps an analysis code action entry kind to its LSP protocol representation.
 * @param[in] kind Analysis code action kind.
 * @return Corresponding LSP CodeActionKind.
 */
static lsp::CodeActionKind ToLspKind(analysis::code_action::CodeActionEntryKind kind)
{
    switch (kind)
    {
    case analysis::code_action::CodeActionEntryKind::QuickFix:
        return lsp::CodeActionKind::QuickFix;
    case analysis::code_action::CodeActionEntryKind::RefactorExtract:
        return lsp::CodeActionKind::RefactorExtract;
    case analysis::code_action::CodeActionEntryKind::RefactorRewrite:
        return lsp::CodeActionKind::RefactorRewrite;
    case analysis::code_action::CodeActionEntryKind::SourceOrganizeImports:
        return lsp::CodeActionKind::SourceOrganizeImports;
    }
    return lsp::CodeActionKind::QuickFix;
}

/**
 * @brief Converts an analysis code action entry into an LSP protocol CodeAction.
 * @param[in,out] entry Analysis code action entry to convert.
 * @param[in] request Original code action request context.
 * @return Fully configured LSP CodeAction.
 */
static lsp::CodeAction ToLspCodeAction(analysis::code_action::CodeActionEntry&& entry, const CodeActionRequest& request)
{
    lsp::CodeAction action;
    action.title = std::move(entry.title);
    action.kind = lsp::CodeActionKindEnum(ToLspKind(entry.kind));

    if (entry.isPreferred.has_value())
    {
        action.isPreferred = *entry.isPreferred;
    }

    if (entry.diagnosticIndices.has_value())
    {
        std::vector<lsp::Diagnostic> diags;
        diags.reserve(entry.diagnosticIndices->size());
        for (uint32_t idx : *entry.diagnosticIndices)
        {
            if (idx < request.context.diagnostics.size())
            {
                diags.push_back(request.context.diagnostics[idx]);
            }
        }
        action.diagnostics = std::move(diags);
    }

    if (entry.edits.has_value())
    {
        std::vector<lsp::TextEdit> textEdits;
        textEdits.reserve(entry.edits->size());
        for (auto& edit : *entry.edits)
        {
            lsp::TextEdit lspEdit;
            lspEdit.range = common::ToLspRange(edit.range);
            lspEdit.newText = std::move(edit.newText);
            textEdits.push_back(std::move(lspEdit));
        }

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = std::move(textEdits);
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);
    }

    if (entry.command.has_value())
    {
        lsp::Command cmd;
        cmd.title = std::move(entry.command->title);
        if (entry.command->kind == analysis::code_action::CodeActionCommandKind::DisableDiagnostic)
        {
            cmd.command = "angelscript.disableDiagnostic";
            lsp::json::Array cmdArgs;
            lsp::json::Object argObj;
            argObj["settingKey"] = lsp::json::Value("angelscript." + entry.command->setting);
            argObj["setting"] = lsp::json::Value(std::move(entry.command->setting));
            argObj["value"] = lsp::json::Value(false);
            argObj["code"] = lsp::json::Value(std::move(entry.command->code));
            cmdArgs.push_back(std::move(argObj));
            cmd.arguments = std::move(cmdArgs);
        }
        else if (entry.command->kind == analysis::code_action::CodeActionCommandKind::IgnoreUnusedSymbol)
        {
            cmd.command = "angelscript.ignoreUnusedSymbol";
            lsp::json::Array cmdArgs;
            lsp::json::Object argObj;
            argObj["symbolName"] = lsp::json::Value(std::move(entry.command->symbolName));
            cmdArgs.push_back(std::move(argObj));
            cmd.arguments = std::move(cmdArgs);
        }
        action.command = std::move(cmd);
    }

    return action;
}

/**
 * @brief Converts and appends analysis code action entries into the LSP actions list.
 * @param[in,out] entries Vector of analysis entries to convert and clear.
 * @param[in] request Original code action request context.
 * @param[out] actions Destination vector for converted LSP actions.
 */
static void FlushEntries(std::vector<analysis::code_action::CodeActionEntry>& entries, const CodeActionRequest& request,
                         std::vector<lsp::CodeAction>& actions)
{
    actions.reserve(actions.size() + entries.size());
    for (auto& entry : entries)
    {
        actions.push_back(ToLspCodeAction(std::move(entry), request));
    }
    entries.clear();
}

std::optional<std::vector<lsp::CodeAction>> GetCodeActions(const CodeActionRequest& request)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    const auto views = BuildDiagnosticViews(request.context);
    const analysis::code_action::CodeActionQuery query{
        request.uri,    request.sourceCode,  request.nodeIndex,  FromLspRange(request.range),
        views,          request.symbolTable, request.scopeIndex, request.allowedRoots,
        request.config, request.i18n};
    std::vector<analysis::code_action::CodeActionEntry> entries;

    std::vector<lsp::CodeAction> actions;
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
    FlushEntries(entries, request, actions);

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
