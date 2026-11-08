/**
 * @file CodeActionUnusedVariables.cpp
 * @brief Quick-fix provider for removing unused local variable declarations.
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "analysis/resolution/TargetResolution.h"
#include "utils/Utils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
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
static CodeActionTextEdit BuildRemoveSingleDeclaratorLine(const analysis::LocalDefinition* def,
                                                          std::string_view sourceCode)
{
    CodeActionTextEdit edit;
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
        edit.range = analysis::SourceRange{def->startLine, 0, def->endLine + 1, 0};
    }
    else
    {
        size_t lastNewline = sourceCode.rfind('\n');
        size_t lastLineLen =
            (lastNewline != std::string::npos) ? (sourceCode.size() - (lastNewline + 1)) : sourceCode.size();
        edit.range = analysis::SourceRange{def->startLine, 0, def->endLine, static_cast<uint32_t>(lastLineLen)};
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
static CodeActionTextEdit BuildRemoveUnusedVariableEdit(const analysis::NodeIndex& index, std::string_view sourceCode,
                                                        const analysis::LocalDefinition* def)
{
    const auto removal = analysis::FindUnusedDeclaratorRemoval(index, def->startLine, def->startCharacter);
    if (!removal.inDeclaration)
    {
        CodeActionTextEdit edit;
        edit.range = analysis::SourceRange{def->startLine, 0, def->endLine + 1, 0};
        edit.newText = "";
        return edit;
    }
    if (removal.declaratorCount <= 1)
    {
        return BuildRemoveSingleDeclaratorLine(def, sourceCode);
    }
    CodeActionTextEdit edit;
    edit.range = removal.multiRemovalRange;
    edit.newText = "";
    return edit;
}

/**
 * @brief Extracts the symbol name associated with an unused symbol diagnostic.
 * @param[in] diag Triggering diagnostic view.
 * @param[in] index Syntax node index, if available.
 * @param[in] sourceCode Source text of the document.
 * @return Extracted symbol name, or empty string if not resolved.
 */
static std::string ExtractSymbolNameFromDiag(const CodeActionDiagnosticView& diag, const analysis::NodeIndex* index,
                                             std::string_view sourceCode)
{
    if (index && !sourceCode.empty())
    {
        const auto span = analysis::FindNodeSpanAtPoint(*index, diag.range.startLine, diag.range.startCharacter);
        if (span)
        {
            const std::string_view text = utils::SliceSource(sourceCode, span->bytes.startByte, span->bytes.endByte);
            if (analysis::IsValidIdentifier(text))
            {
                return std::string(text);
            }
        }
    }
    const std::string_view msgStr = diag.message;
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
 * @brief Constructs a QuickFix entry to ignore an unused symbol via workspace config.
 * @param[in] diagIndex Triggering diagnostic index.
 * @param[in] symbolName Identifier name of the unused symbol.
 * @param[in] i18n Internationalization provider for localized action titles.
 * @return Configured CodeActionEntry ready for presentation.
 */
static CodeActionEntry MakeIgnoreUnusedSymbolEntry(uint32_t diagIndex, std::string_view symbolName,
                                                   const i18n::I18n* i18n)
{
    std::string title = i18n::FormatMessage(i18n, "action-ignore-unused-symbol",
                                            "Ignore unused symbol '{}' (mark as entry point / callback)", symbolName);
    std::string cmdTitle = i18n::FormatMessage(i18n, "action-ignore-unused-symbol-cmd", "Ignore unused symbol");

    CodeActionEntry entry;
    entry.title = std::move(title);
    entry.kind = CodeActionEntryKind::QuickFix;
    entry.isPreferred = true;
    entry.diagnosticIndices = std::vector<uint32_t>{diagIndex};

    CodeActionCommandEntry cmd;
    cmd.kind = CodeActionCommandKind::IgnoreUnusedSymbol;
    cmd.title = std::move(cmdTitle);
    cmd.symbolName = std::string(symbolName);
    entry.command = std::move(cmd);

    return entry;
}

/**
 * @brief Appends quick-fix entries for unused symbol diagnostics.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination entries vector.
 */
static void AppendUnusedSymbolEntries(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedFunction))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, query.nodeIndex, query.sourceCode);
            if (!symName.empty())
            {
                entries.push_back(MakeIgnoreUnusedSymbolEntry(i, symName, query.i18n));
            }
            entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
                i, "diagnostics.reportUnusedFunctions", std::string(diagnostics::codes::UnusedFunction), query.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedMethod))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, query.nodeIndex, query.sourceCode);
            if (!symName.empty())
            {
                entries.push_back(MakeIgnoreUnusedSymbolEntry(i, symName, query.i18n));
            }
            entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
                i, "diagnostics.reportUnusedMembers", std::string(diagnostics::codes::UnusedMethod), query.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedField))
        {
            entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
                i, "diagnostics.reportUnusedMembers", std::string(diagnostics::codes::UnusedField), query.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedGlobalVariable))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, query.nodeIndex, query.sourceCode);
            if (!symName.empty())
            {
                entries.push_back(MakeIgnoreUnusedSymbolEntry(i, symName, query.i18n));
            }
            entries.push_back(MakeDisableDiagnosticEntry(
                DisableDiagnosticEntryOptions{i, "diagnostics.reportUnusedGlobalVariables",
                                              std::string(diagnostics::codes::UnusedGlobalVariable), query.i18n}));
        }
        else if (MatchDiagnosticCode(diag, diagnostics::codes::UnusedClass))
        {
            std::string symName = ExtractSymbolNameFromDiag(diag, query.nodeIndex, query.sourceCode);
            if (!symName.empty())
            {
                entries.push_back(MakeIgnoreUnusedSymbolEntry(i, symName, query.i18n));
            }
            entries.push_back(MakeDisableDiagnosticEntry(DisableDiagnosticEntryOptions{
                i, "diagnostics.reportUnusedClasses", std::string(diagnostics::codes::UnusedClass), query.i18n}));
        }
    }
}

/**
 * @brief Generates quick-fix entries to remove unused variables and suppress unused symbol diagnostics.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddRemoveUnusedVariableFixes(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    AppendUnusedSymbolEntries(query, entries);

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    if (!rootScope)
    {
        return;
    }

    ankerl::unordered_dense::set<const analysis::LocalDefinition*> usedDefs;
    CollectUsedDefinitions(rootScope.get(), usedDefs);

    std::vector<const analysis::LocalDefinition*> unusedVars;
    CollectUnusedVariables(rootScope.get(), usedDefs, unusedVars);

    if (!query.nodeIndex)
    {
        return;
    }

    for (const auto* def : unusedVars)
    {
        bool matchesRange = (query.range.startLine <= def->endLine && query.range.endLine >= def->startLine);
        bool matchesDiag = false;

        for (const auto& diag : query.diagnostics)
        {
            if (diag.range.startLine <= def->endLine && diag.range.endLine >= def->startLine)
            {
                matchesDiag = true;
                break;
            }
        }

        if (!matchesRange && !matchesDiag)
        {
            continue;
        }

        CodeActionTextEdit edit = BuildRemoveUnusedVariableEdit(*query.nodeIndex, query.sourceCode, def);
        CodeActionEntry entry;
        entry.title = i18n::FormatMessage(query.i18n, "action-remove-unused-variable", "Remove unused variable '{}'",
                                          std::string_view(def->name));
        entry.kind = CodeActionEntryKind::QuickFix;
        entry.isPreferred = true;
        entry.edits = std::vector<CodeActionTextEdit>{std::move(edit)};

        std::vector<uint32_t> matchingDiagIndices;
        for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
        {
            const auto& diag = query.diagnostics[i];
            if (diag.range.startLine <= def->endLine && diag.range.endLine >= def->startLine)
            {
                matchingDiagIndices.push_back(i);
            }
        }
        if (!matchingDiagIndices.empty())
        {
            entry.diagnosticIndices = std::move(matchingDiagIndices);
        }

        entries.push_back(std::move(entry));
    }
}

} // namespace angel_lsp::analysis::code_action
