/**
 * @file CodeActionConstQualifier.cpp
 * @brief Code action providers for adding const qualifier to methods.
 */

#include "analysis/index/ConstQualifierFacts.h"
#include "analysis/resolution/CodeActionResolution.h"
#include "analysis/types/TypeSanitization.h"
#include "features/code_action/CodeActionClassMutation.h"
#include "features/code_action/CodeActionInternal.h"
#include "utils/Utils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Attempts to construct a QuickFix CodeAction to add a const qualifier to a referenced method declaration.
 * @param[in] request Code action request bundle containing source text, node index, and parameters.
 * @param[in] diag LSP diagnostic reporting the missing const method call error.
 * @param[in] sym Target method symbol needing const qualification.
 * @return QuickFix CodeAction if method declaration node is located, or std::nullopt otherwise.
 */
static std::optional<lsp::CodeAction> TryBuildMissingConstFixForSymbol(const CodeActionRequest& request,
                                                                       const lsp::Diagnostic& diag,
                                                                       const analysis::Symbol& sym)
{
    const auto insertPt = analysis::FindMethodParameterListEnd(*request.nodeIndex, sym.startLine, sym.startCharacter);
    if (!insertPt)
    {
        return std::nullopt;
    }

    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt->line, insertPt->character}, {insertPt->line, insertPt->character}};
    edit.newText = " const";

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = {diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

/**
 * @brief Constructs missing const quick-fix actions for call sites diagnosed with const mismatch.
 * @param[in] request Code action request bundle containing syntax facts and symbol table.
 * @param[in] diag LSP diagnostic reporting the missing const call site.
 * @param[in,out] actions Output list where constructed CodeActions are appended.
 */
static void TryAddMissingConstDiagnosticFix(const CodeActionRequest& request, const lsp::Diagnostic& diag,
                                            std::vector<lsp::CodeAction>& actions)
{
    const auto site =
        analysis::FindConstCallSite(*request.nodeIndex, diag.range.start.line, diag.range.start.character);
    const std::string methodName(
        utils::SliceSource(request.sourceCode, site.methodName.startByte, site.methodName.endByte));

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), diag.range.start.line);
    std::string objType =
        site.receiver.Empty()
            ? std::string()
            : analysis::CleanBaseType(analysis::ResolveExpressionTypeAt(
                  site.receiver, {scope, request.symbolTable, request.sourceCode, request.uri, request.nodeIndex}));

    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.type == analysis::SymbolType::Function && sym.name == methodName &&
                    (objType.empty() || sym.containerName == objType) && sym.fileUri == request.uri)
                {
                    if (auto action = TryBuildMissingConstFixForSymbol(request, diag, sym))
                    {
                        actions.push_back(std::move(*action));
                    }
                }
            }
        });
}

/**
 * @brief Constructs a QuickFix CodeAction to append the 'const' qualifier to a method parameter list.
 * @param[in] insertPt Source coordinates after the method parameter list where 'const' is inserted.
 * @param[in] request Contextual request bundle containing source text and document parameters.
 * @return QuickFix CodeAction with text edit inserting ' const'.
 */
static lsp::CodeAction BuildAddConstAction(analysis::IndexedSourcePoint insertPt, const CodeActionRequest& request)
{
    lsp::TextEdit edit;
    edit.range = lsp::Range{{insertPt.line, insertPt.character}, {insertPt.line, insertPt.character}};
    edit.newText = " const";

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    return action;
}

/**
 * @brief Evaluates whether a non-mutating method qualifies for intentional const qualification.
 * @param[in] request Code action request bundle containing position, source text, and symbol table.
 * @param[in,out] actions Output list where constructed CodeActions are appended.
 */
static void TryAddIntentionalConstAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    const auto site = analysis::FindIntentionalConstSite(*request.nodeIndex, request.sourceCode,
                                                         request.range.start.line, request.range.start.character);
    if (!site)
    {
        return;
    }

    const std::string className(
        utils::SliceSource(request.sourceCode, site->className.startByte, site->className.endByte));

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), site->methodStartLine);

    ClassMutationContext mutCtx{site->mutation, request.sourceCode, request.symbolTable, className, scope};
    if (!MethodBodyMutatesClassState(mutCtx))
    {
        actions.push_back(BuildAddConstAction(site->parameterListEnd, request));
    }
}

/**
 * @brief Constructs a QuickFix action to prepend 'const' to a handle variable declaration.
 * @param[in] request Contextual request bundle containing source text and parameters.
 * @param[in] diag LSP Diagnostic triggering the const qualifier action.
 * @param[in,out] actions Output list where constructed CodeAction is appended.
 */
static void TryAddConstHandleDiagnosticFix(const CodeActionRequest& request, const lsp::Diagnostic& diag,
                                           std::vector<lsp::CodeAction>& actions)
{
    const auto site =
        analysis::FindConstHandleSite(*request.nodeIndex, diag.range.start.line, diag.range.start.character);
    if (!site)
    {
        return;
    }

    const std::string_view typeText =
        utils::SliceSource(request.sourceCode, site->typeText.startByte, site->typeText.endByte);
    if (!analysis::IsHandleTypeView(typeText) || analysis::IsConstTypeView(typeText))
    {
        return;
    }

    lsp::TextEdit edit;
    edit.range = lsp::Range{{site->typeStart.line, site->typeStart.character},
                            {site->typeStart.line, site->typeStart.character}};
    edit.newText = "const ";

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-add-const-variable-qualifier", "Add 'const' qualifier");
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = {diag};

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);
    actions.push_back(std::move(action));
}

void TryAddConstQualifierActions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return;
    }
    for (const auto& diag : request.context.diagnostics)
    {
        if (MatchDiagnosticCode(diag, "as-err-const-method-required"))
        {
            TryAddMissingConstDiagnosticFix(request, diag, actions);
        }
        else if (MatchDiagnosticCode(diag, "as-err-no-implicit-conversion"))
        {
            TryAddConstHandleDiagnosticFix(request, diag, actions);
        }
    }
    TryAddIntentionalConstAction(request, actions);
}

} // namespace angel_lsp::features
