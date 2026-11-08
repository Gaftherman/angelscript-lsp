#pragma once

#include "analysis/code_action/CodeActionTypes.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/CodeActionFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "features/code_action/CodeActionHandler.h"
#include "parser/GrammarNames.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Extracts leading whitespace indentation of a given 0-indexed line.
 * @param[in] sourceCode Source document text.
 * @param[in] line 0-based target line index.
 * @return Leading indentation string.
 */
std::string GetLineIndentation(std::string_view sourceCode, uint32_t line);

/**
 * @brief Locates the innermost lexical scope containing the given line.
 * @param[in] root Root scope node.
 * @param[in] line 0-based line number.
 * @return Innermost matching scope or root if uncontained.
 */
const analysis::Scope* FindScopeByLine(const analysis::Scope* root, uint32_t line);

/**
 * @brief Locates the innermost lexical scope containing the given point, falling back to root.
 * @param[in] root Root scope node.
 * @param[in] line 0-based line number.
 * @return Innermost scope or root if none matches.
 */
const analysis::Scope* FindScopeByLineOrRoot(const analysis::Scope* root, uint32_t line);

/**
 * @brief Checks if a diagnostic view matches an expected error/warning code string.
 * @param[in] diag Diagnostic view object.
 * @param[in] expectedCode Code string to match.
 * @return True if diagnostic code matches.
 */
bool MatchDiagnosticCode(const analysis::code_action::CodeActionDiagnosticView& diag, std::string_view expectedCode);

/**
 * @brief Bounded Levenshtein edit distance between two strings.
 * @param[in] a First string.
 * @param[in] b Second string.
 * @param[in] limit Maximum search distance threshold.
 * @return Computed distance or limit + 1 if exceeded.
 */
size_t BoundedEditDistance(std::string_view a, std::string_view b, size_t limit);

/**
 * @brief Determines maximum allowed typo distance by identifier length.
 * @param[in] nameLength Length of identifier.
 * @return Allowed edit distance.
 */
size_t SuggestionLimit(size_t nameLength);

/**
 * @brief Converts ASCII string to lowercase for case-insensitive matching.
 * @param[in] text Input string slice.
 * @return Folded lowercase string.
 */
std::string FoldCase(std::string_view text);

/**
 * @brief Collects all identifier reference names across the scope hierarchy using a worklist.
 * @param[in] rootScope Root of scope subtree.
 * @param[out] refs Destination reference name set.
 */
void CollectAllReferences(const analysis::Scope* rootScope, ankerl::unordered_dense::set<std::string>& refs);

/**
 * @brief Candidate name with an associated edit distance for typo and include suggestions.
 */
struct TypoCandidate
{
    std::string name;
    size_t distance = 0;
};

/**
 * @brief Extracted diagnostic parameters for repeated conversion warnings.
 */
struct ExtractedDiagInfo
{
    std::string exprText;
    std::string fromType;
    std::string toType;
};

/**
 * @brief Options bundle for creating a diagnostic disabling quick-fix entry.
 */
struct DisableDiagnosticEntryOptions
{
    uint32_t diagnosticIndex = 0;
    std::string settingKey;
    std::string code;
    const i18n::I18n* i18n = nullptr;
    std::string title;
};

/**
 * @brief Constructs a QuickFix entry that disables a configurable diagnostic in workspace settings.
 * @param[in] options Options bundle containing diagnostic index, settingKey, code, and optional title/i18n.
 * @return Constructed CodeActionEntry with command entry payload.
 */
analysis::code_action::CodeActionEntry MakeDisableDiagnosticEntry(const DisableDiagnosticEntryOptions& options);

/**
 * @brief Options bundle for creating a text edit quick-fix code action entry.
 */
struct QuickFixEntryOptions
{
    std::string title;
    uint32_t diagnosticIndex = 0;
    std::vector<analysis::code_action::CodeActionTextEdit> edits;
    bool isPreferred = false;
};

/**
 * @brief Constructs an analysis CodeActionEntry with the provided text edits.
 * @param[in] options Quick fix configuration bundle.
 * @return Constructed CodeActionEntry.
 */
analysis::code_action::CodeActionEntry MakeQuickFixEntry(QuickFixEntryOptions options);

// Feature Provider Function Declarations
/**
 * @brief Generates quick-fix entries to remove unused variables and suppress unused symbol diagnostics.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddRemoveUnusedVariableFixes(const analysis::code_action::CodeActionQuery& query,
                                     std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries for implementing missing interface methods in classes.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddImplementInterfaceFixes(const analysis::code_action::CodeActionQuery& query,
                                   std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates a refactoring entry to extract an expression into a local variable.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddExtractVariableAction(const analysis::code_action::CodeActionQuery& query,
                                 std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates a refactoring entry to extract statements into a method.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddExtractMethodAction(const analysis::code_action::CodeActionQuery& query,
                               std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates refactoring entries to create getter and setter methods for class fields.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddGetterSetterActions(const analysis::code_action::CodeActionQuery& query,
                               std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries adding const qualifiers to methods and variables.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddConstQualifierActions(const analysis::code_action::CodeActionQuery& query,
                                 std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries suggesting corrections for undefined identifiers and unknown types.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddUndefinedIdentifierSuggestions(const analysis::code_action::CodeActionQuery& query,
                                          std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries to remove illegal handle modifiers from primitive types.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddHandleOnPrimitiveFix(const analysis::code_action::CodeActionQuery& query,
                                std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries suggesting corrections for unresolved include directives.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddUnresolvedIncludeSuggestions(const analysis::code_action::CodeActionQuery& query,
                                        std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries adding the property keyword to accessor methods.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddAccessorPropertyKeywordFix(const analysis::code_action::CodeActionQuery& query,
                                      std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries for explicit bool conversion calls and diagnostic suppression.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddBoolConversionFix(const analysis::code_action::CodeActionQuery& query,
                             std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries to cache repeated conversions in an if-ladder.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddRepeatedConversionFix(const analysis::code_action::CodeActionQuery& query,
                                 std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries to declare missing funcdef signatures.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddGenerateFuncdefFix(const analysis::code_action::CodeActionQuery& query,
                              std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates a source action to sort and remove unreferenced include directives.
 * @param query Incoming code action query context.
 * @param entries Destination vector for generated code action entries.
 */
void TryAddSortAndCleanIncludesAction(const analysis::code_action::CodeActionQuery& query,
                                      std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Generates quick-fix entries to suppress diagnostics via comments.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddDiagnosticSuppressionFixes(const analysis::code_action::CodeActionQuery& query,
                                      std::vector<analysis::code_action::CodeActionEntry>& entries);

/**
 * @brief Carries accessor generation parameters including property name, type descriptor, and enclosing class scope.
 */
struct GetterSetterContext
{
    std::string className;
    const std::pair<std::string, std::string>& field;
    analysis::IndexedSourcePoint insertPt;
};

/**
 * @brief Context for generating automatic #include insertion code actions for undefined symbols.
 */
struct IncludeFixContext
{
    const analysis::code_action::CodeActionQuery& query; ///< Incoming code action query context.
    uint32_t diagIndex = 0;                              ///< Index of triggering diagnostic in query diagnostics.
    const angel_lsp::utils::IncludeDirective& directive; ///< Target include directive being resolved.
};

/**
 * @brief Parameters for checking whether an unresolved symbol can be satisfied by an available include header.
 */
struct IncludeRefCheckContext
{
    std::string currentUri;
    const analysis::SymbolTable& table;
    const ankerl::unordered_dense::set<std::string>& docReferences;
    std::span<const std::string> allowedRoots;
};

/**
 * @brief Code action proposal for fixing mismatched funcdef signatures or missing declarations.
 */
struct FuncdefFixItem
{
    analysis::SourceRange typeRange;
    std::string functionName;
    std::string funcdefName;
    std::string declaration;
};

/**
 * @brief Context holding candidate symbol names and Levenshtein distances for typo correction code actions.
 */
struct TypoFixContext
{
    const analysis::code_action::CodeActionQuery& query; ///< Incoming code action query context.
    uint32_t diagIndex = 0;                              ///< Index of triggering diagnostic in query diagnostics.
    analysis::SourceRange identifierRange;               ///< Source range of unresolved identifier token.
};

/**
 * @brief Manages an unused variable or parameter candidate within a lexical scope state and operations.
 */
struct ScopeItem
{
    const analysis::Scope* scope;
    bool isFuncNested;
};
} // namespace angel_lsp::features
