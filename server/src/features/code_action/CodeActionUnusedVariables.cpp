/**
 * @file CodeActionUnusedVariables.cpp
 * @brief Quick-fix provider for removing unused local variable declarations.
 */

#include "analysis/resolution/TargetResolution.h"
#include "features/code_action/CodeActionInternal.h"
#include "features/common/FeatureSharedUtils.h"
#include "utils/Utils.h"

#include <variant>

namespace angel_lsp::features
{

/**
 * @brief Traverses scopes with a worklist to collect referenced definitions.
 * @param[in] rootScope Root of scope subtree.
 * @param[out] used Destination set of referenced definitions.
 */
static void CollectUsedDefinitions(const analysis::Scope* rootScope,
                                   ankerl::unordered_dense::set<const analysis::LocalDefinition*>& used)
{
    if (!rootScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> worklist = {rootScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();

        for (const auto& ref : sc->references)
        {
            if (ref.isMemberAccess || ref.isNamedArgument)
            {
                continue;
            }
            const analysis::LocalDefinition* def = analysis::ResolveInScope(sc, ref.name);
            if (!def)
            {
                continue;
            }
            if (def->startLine == ref.startLine && def->startCharacter == ref.startCharacter &&
                def->endLine == ref.endLine && def->endCharacter == ref.endCharacter)
            {
                continue;
            }
            used.insert(def);
        }

        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Traverses scopes with a worklist to collect unused local variables.
 * @param[in] rootScope Root of scope subtree.
 * @param[in] used Set of referenced definitions.
 * @param[out] unused Destination vector for unused variable definitions.
 */
static void CollectUnusedVariables(const analysis::Scope* rootScope,
                                   const ankerl::unordered_dense::set<const analysis::LocalDefinition*>& used,
                                   std::vector<const analysis::LocalDefinition*>& unused)
{
    if (!rootScope)
    {
        return;
    }
    std::vector<ScopeItem> worklist = {{rootScope, rootScope->isFunctionScope}};

    while (!worklist.empty())
    {
        auto [sc, funcNested] = worklist.back();
        worklist.pop_back();

        bool nested = funcNested || sc->isFunctionScope;
        if (nested)
        {
            for (const auto& def : sc->definitions)
            {
                if (def.kind == analysis::LocalDefinitionKind::Variable && !used.contains(&def))
                {
                    unused.push_back(&def);
                }
            }
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back({child.get(), nested});
        }
    }
}

/**
 * @brief Constructs a TextEdit to delete an entire declaration line.
 * @param[in] def Local variable definition.
 * @param[in] sourceCode Source text.
 * @return TextEdit deleting the line range.
 */
static lsp::TextEdit BuildRemoveSingleDeclaratorLine(const analysis::LocalDefinition* def, std::string_view sourceCode)
{
    lsp::TextEdit edit;
    uint32_t lineCount = 1;
    for (char c : sourceCode)
    {
        if (c == '\n')
        {
            lineCount++;
        }
    }
    if (def->endLine + 1 < lineCount)
    {
        edit.range.start = lsp::Position{def->startLine, 0};
        edit.range.end = lsp::Position{def->endLine + 1, 0};
    }
    else
    {
        edit.range.start = lsp::Position{def->startLine, 0};
        size_t lastNewline = sourceCode.rfind('\n');
        size_t lastLineLen =
            (lastNewline != std::string::npos) ? (sourceCode.size() - (lastNewline + 1)) : sourceCode.size();
        edit.range.end = lsp::Position{def->endLine, static_cast<uint32_t>(lastLineLen)};
    }
    edit.newText = "";
    return edit;
}

/**
 * @brief Constructs a TextEdit to delete an unused variable declaration or declarator.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] sourceCode Source text.
 * @param[in] def Target local definition.
 * @return TextEdit deleting the unused variable.
 */
static lsp::TextEdit BuildRemoveUnusedVariableEdit(const analysis::NodeIndex& index, std::string_view sourceCode,
                                                   const analysis::LocalDefinition* def)
{
    const auto removal = analysis::FindUnusedDeclaratorRemoval(index, def->startLine, def->startCharacter);
    if (!removal.inDeclaration)
    {
        lsp::TextEdit edit;
        edit.range.start = lsp::Position{def->startLine, 0};
        edit.range.end = lsp::Position{def->endLine + 1, 0};
        edit.newText = "";
        return edit;
    }
    if (removal.declaratorCount <= 1)
    {
        return BuildRemoveSingleDeclaratorLine(def, sourceCode);
    }
    lsp::TextEdit edit;
    edit.range = common::ToLspRange(removal.multiRemovalRange);
    edit.newText = "";
    return edit;
}

static std::string ExtractSymbolNameFromDiag(const lsp::Diagnostic& diag, const analysis::NodeIndex* index,
                                             std::string_view sourceCode)
{
    if (index && !sourceCode.empty())
    {
        const auto span = analysis::FindNodeSpanAtPoint(*index, diag.range.start.line, diag.range.start.character);
        if (span)
        {
            const std::string_view text = utils::SliceSource(sourceCode, span->bytes.startByte, span->bytes.endByte);
            if (analysis::IsValidIdentifier(text))
            {
                return std::string(text);
            }
        }
    }
    std::string_view msgStr;
    if (const auto* s = std::get_if<lsp::String>(&diag.message))
    {
        msgStr = *s;
    }
    else if (const auto* m = std::get_if<lsp::MarkupContent>(&diag.message))
    {
        msgStr = m->value;
    }
    auto firstQuote = msgStr.find('\'');
    if (firstQuote != std::string_view::npos)
    {
        auto secondQuote = msgStr.find('\'', firstQuote + 1);
        if (secondQuote != std::string_view::npos && secondQuote > firstQuote + 1)
        {
            return std::string(msgStr.substr(firstQuote + 1, secondQuote - firstQuote - 1));
        }
    }
    return {};
}

/**
 * @brief Constructs a QuickFix CodeAction to ignore an unused symbol via workspace config.
 * @param[in] diag LSP Diagnostic reporting the unused symbol.
 * @param[in] symbolName Identifier name of the unused symbol.
 * @param[in] i18n Internationalization provider for localized action titles.
 * @return Configured CodeAction ready for client presentation.
 */
static lsp::CodeAction MakeIgnoreUnusedSymbolAction(const lsp::Diagnostic& diag, std::string_view symbolName,
                                                    const i18n::I18n* i18n)
{
    std::string title = i18n::FormatMessage(i18n, "action-ignore-unused-symbol",
                                            "Ignore unused symbol '{}' (mark as entry point / callback)", symbolName);
    std::string cmdTitle = i18n::FormatMessage(i18n, "action-ignore-unused-symbol-cmd", "Ignore unused symbol");

    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
    action.isPreferred = true;
    action.diagnostics = std::vector<lsp::Diagnostic>{diag};

    lsp::Command cmd;
    cmd.title = std::move(cmdTitle);
    cmd.command = "angelscript.ignoreUnusedSymbol";
    lsp::json::Array cmdArgs;
    lsp::json::Object argObj;
    argObj["symbolName"] = lsp::json::Value(std::string(symbolName));
    cmdArgs.push_back(std::move(argObj));
    cmd.arguments = std::move(cmdArgs);

    action.command = std::move(cmd);
    return action;
}

static void AppendUnusedSymbolActions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    for (const auto& diag : request.context.diagnostics)
    {
        if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedFunction))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, request.nodeIndex, request.sourceCode);
            if (!symName.empty())
            {
                actions.push_back(MakeIgnoreUnusedSymbolAction(diag, symName, request.i18n));
            }
            actions.push_back(
                MakeDisableDiagnosticAction({diag, "diagnostics.reportUnusedFunctions",
                                             std::string(diagnostics::codes::UnusedFunction), request.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedMethod))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, request.nodeIndex, request.sourceCode);
            if (!symName.empty())
            {
                actions.push_back(MakeIgnoreUnusedSymbolAction(diag, symName, request.i18n));
            }
            actions.push_back(
                MakeDisableDiagnosticAction({diag, "diagnostics.reportUnusedMembers",
                                             std::string(diagnostics::codes::UnusedMethod), request.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedField))
        {
            actions.push_back(MakeDisableDiagnosticAction(
                {diag, "diagnostics.reportUnusedMembers", std::string(diagnostics::codes::UnusedField), request.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedGlobalVariable))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, request.nodeIndex, request.sourceCode);
            if (!symName.empty())
            {
                actions.push_back(MakeIgnoreUnusedSymbolAction(diag, symName, request.i18n));
            }
            actions.push_back(
                MakeDisableDiagnosticAction({diag, "diagnostics.reportUnusedGlobalVariables",
                                             std::string(diagnostics::codes::UnusedGlobalVariable), request.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedClass))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, request.nodeIndex, request.sourceCode);
            if (!symName.empty())
            {
                actions.push_back(MakeIgnoreUnusedSymbolAction(diag, symName, request.i18n));
            }
            actions.push_back(MakeDisableDiagnosticAction(
                {diag, "diagnostics.reportUnusedClasses", std::string(diagnostics::codes::UnusedClass), request.i18n}));
        }
    }
}

void TryAddRemoveUnusedVariableFixes(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    AppendUnusedSymbolActions(request, actions);

    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    if (!rootScope)
    {
        return;
    }

    ankerl::unordered_dense::set<const analysis::LocalDefinition*> usedDefs;
    CollectUsedDefinitions(rootScope.get(), usedDefs);

    std::vector<const analysis::LocalDefinition*> unusedVars;
    CollectUnusedVariables(rootScope.get(), usedDefs, unusedVars);

    if (!request.nodeIndex)
    {
        return;
    }

    for (const auto* def : unusedVars)
    {
        bool matchesRange = (request.range.start.line <= def->endLine && request.range.end.line >= def->startLine);
        bool matchesDiag = false;

        for (const auto& diag : request.context.diagnostics)
        {
            if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
            {
                matchesDiag = true;
                break;
            }
        }

        if (!matchesRange && !matchesDiag)
        {
            continue;
        }

        lsp::TextEdit edit = BuildRemoveUnusedVariableEdit(*request.nodeIndex, request.sourceCode, def);
        lsp::CodeAction action;
        action.title = i18n::FormatMessage(request.i18n, "action-remove-unused-variable", "Remove unused variable '{}'",
                                           std::string_view(def->name));
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.isPreferred = true;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        std::vector<lsp::Diagnostic> matchingDiags;
        for (const auto& diag : request.context.diagnostics)
        {
            if (diag.range.start.line <= def->endLine && diag.range.end.line >= def->startLine)
            {
                matchingDiags.push_back(diag);
            }
        }
        if (!matchingDiags.empty())
        {
            action.diagnostics = std::move(matchingDiags);
        }

        actions.push_back(std::move(action));
    }
}

} // namespace angel_lsp::features
