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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Attempts to construct a QuickFix CodeActionEntry to add a const qualifier to a referenced method.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Index of the triggering diagnostic in query diagnostics.
 * @param[in] sym Target method symbol needing const qualification.
 * @return QuickFix CodeActionEntry if method declaration node is located, or std::nullopt otherwise.
 */
static std::optional<analysis::code_action::CodeActionEntry>
TryBuildMissingConstFixForSymbol(const analysis::code_action::CodeActionQuery& query, uint32_t diagIndex,
                                 const analysis::Symbol& sym)
{
    const auto insertPt = analysis::FindMethodParameterListEnd(*query.nodeIndex, sym.startLine, sym.startCharacter);
    if (!insertPt)
    {
        return std::nullopt;
    }

    std::string title =
        i18n::FormatMessage(query.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");

    return MakeQuickFixEntry(QuickFixEntryOptions{
        std::move(title),
        diagIndex,
        {analysis::code_action::CodeActionTextEdit{
            analysis::SourceRange{insertPt->line, insertPt->character, insertPt->line, insertPt->character}, " const"}},
        true});
}

/**
 * @brief Constructs missing const quick-fix entries for call sites diagnosed with const mismatch.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Index of the triggering diagnostic in query diagnostics.
 * @param[in,out] entries Output list where constructed CodeActionEntries are appended.
 */
static void TryAddMissingConstDiagnosticFix(const analysis::code_action::CodeActionQuery& query, uint32_t diagIndex,
                                            std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    const auto& diag = query.diagnostics[diagIndex];
    const auto site = analysis::FindConstCallSite(*query.nodeIndex, diag.range.startLine, diag.range.startCharacter);
    const std::string methodName(
        utils::SliceSource(query.sourceCode, site.methodName.startByte, site.methodName.endByte));

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), diag.range.startLine);
    std::string objType =
        site.receiver.Empty()
            ? std::string()
            : analysis::CleanBaseType(analysis::ResolveExpressionTypeAt(
                  site.receiver, {scope, query.symbolTable, query.sourceCode, query.uri, query.nodeIndex}));

    query.symbolTable.ForEachSymbolInFile(
        query.uri,
        [&query, diagIndex, &entries, &methodName, &objType]([[maybe_unused]] const std::string& qualifiedName,
                                                             const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& sym : symList)
            {
                if (sym.type == analysis::SymbolType::Function && sym.name == methodName &&
                    (objType.empty() || sym.containerName == objType) && sym.fileUri == query.uri)
                {
                    if (auto entry = TryBuildMissingConstFixForSymbol(query, diagIndex, sym))
                    {
                        entries.push_back(std::move(*entry));
                    }
                }
            }
        });
}

/**
 * @brief Constructs a QuickFix CodeActionEntry to append the 'const' qualifier to a method parameter list.
 * @param[in] insertPt Source coordinates after the method parameter list where 'const' is inserted.
 * @param[in] query Incoming code action query context.
 * @return QuickFix CodeActionEntry with text edit inserting ' const'.
 */
static analysis::code_action::CodeActionEntry BuildAddConstEntry(analysis::IndexedSourcePoint insertPt,
                                                                 const analysis::code_action::CodeActionQuery& query)
{
    analysis::code_action::CodeActionEntry entry;
    entry.title = i18n::FormatMessage(query.i18n, "action-add-const-qualifier", "Add 'const' qualifier to method");
    entry.kind = analysis::code_action::CodeActionEntryKind::QuickFix;
    entry.edits = std::vector<analysis::code_action::CodeActionTextEdit>{
        analysis::code_action::CodeActionTextEdit{
            analysis::SourceRange{insertPt.line, insertPt.character, insertPt.line, insertPt.character},
            " const",
        },
    };
    return entry;
}

/**
 * @brief Evaluates whether a non-mutating method qualifies for intentional const qualification.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Output list where constructed CodeActionEntries are appended.
 */
static void TryAddIntentionalConstAction(const analysis::code_action::CodeActionQuery& query,
                                         std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    const auto site = analysis::FindIntentionalConstSite(*query.nodeIndex, query.sourceCode, query.range.startLine,
                                                         query.range.startCharacter);
    if (!site)
    {
        return;
    }

    const std::string className(
        utils::SliceSource(query.sourceCode, site->className.startByte, site->className.endByte));

    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), site->methodStartLine);

    ClassMutationContext mutCtx{site->mutation, query.sourceCode, query.symbolTable, className, scope};
    if (!MethodBodyMutatesClassState(mutCtx))
    {
        entries.push_back(BuildAddConstEntry(site->parameterListEnd, query));
    }
}

/**
 * @brief Constructs a QuickFix entry to prepend 'const' to a handle variable declaration.
 * @param[in] query Incoming code action query context.
 * @param[in] diagIndex Index of the triggering diagnostic in query diagnostics.
 * @param[in,out] entries Output list where constructed CodeActionEntries are appended.
 */
static void TryAddConstHandleDiagnosticFix(const analysis::code_action::CodeActionQuery& query, uint32_t diagIndex,
                                           std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    const auto& diag = query.diagnostics[diagIndex];
    const auto site = analysis::FindConstHandleSite(*query.nodeIndex, diag.range.startLine, diag.range.startCharacter);
    if (!site)
    {
        return;
    }

    const std::string_view typeText =
        utils::SliceSource(query.sourceCode, site->typeText.startByte, site->typeText.endByte);
    if (!analysis::IsHandleTypeView(typeText) || analysis::IsConstTypeView(typeText))
    {
        return;
    }

    std::string title = i18n::FormatMessage(query.i18n, "action-add-const-variable-qualifier", "Add 'const' qualifier");

    entries.push_back(MakeQuickFixEntry(
        QuickFixEntryOptions{std::move(title),
                             diagIndex,
                             {analysis::code_action::CodeActionTextEdit{
                                 analysis::SourceRange{site->typeStart.line, site->typeStart.character,
                                                       site->typeStart.line, site->typeStart.character},
                                 "const "}},
                             true}));
}

/**
 * @brief Generates quick-fix entries adding const qualifiers to methods and variables.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddConstQualifierActions(const analysis::code_action::CodeActionQuery& query,
                                 std::vector<analysis::code_action::CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (MatchDiagnosticCode(diag, "as-err-const-method-required"))
        {
            TryAddMissingConstDiagnosticFix(query, i, entries);
        }
        else if (MatchDiagnosticCode(diag, "as-err-no-implicit-conversion"))
        {
            TryAddConstHandleDiagnosticFix(query, i, entries);
        }
    }
    TryAddIntentionalConstAction(query, entries);
}

} // namespace angel_lsp::features
