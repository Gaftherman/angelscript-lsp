#pragma once

#include "features/code_action/CodeActionHandler.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolTable.h"
#include "parser/GrammarNames.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Extracts text slice of an AST node from the source code.
 * @param[in] node Tree-sitter AST node.
 * @param[in] sourceCode Source document text.
 * @return Extracted node text.
 */
std::string GetNodeText(TSNode node, std::string_view sourceCode);

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
 * @brief Checks if a diagnostic matches an expected error/warning code string.
 * @param[in] diag Diagnostic object.
 * @param[in] expectedCode Code string to match.
 * @return True if diagnostic code matches.
 */
bool MatchDiagnosticCode(const lsp::Diagnostic& diag, std::string_view expectedCode);

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

// Feature Provider Function Declarations
void TryAddRemoveUnusedVariableFixes(const CodeActionRequest& request, TSNode rootNode,
                                    std::vector<lsp::CodeAction>& actions);

void TryAddImplementInterfaceFixes(const CodeActionRequest& request, TSNode rootNode,
                                   std::vector<lsp::CodeAction>& actions);

void TryAddExtractVariableAction(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions);

void TryAddExtractMethodAction(const CodeActionRequest& request, TSNode rootNode,
                               std::vector<lsp::CodeAction>& actions);

void TryAddGetterSetterActions(const CodeActionRequest& request, TSNode rootNode,
                               std::vector<lsp::CodeAction>& actions);

void TryAddConstQualifierActions(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions);

void TryAddUndefinedIdentifierSuggestions(const CodeActionRequest& request, TSNode rootNode,
                                          std::vector<lsp::CodeAction>& actions);

void TryAddHandleOnPrimitiveFix(const CodeActionRequest& request, TSNode rootNode,
                                std::vector<lsp::CodeAction>& actions);

void TryAddUnresolvedIncludeSuggestions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions);

void TryAddAccessorPropertyKeywordFix(const CodeActionRequest& request, TSNode rootNode,
                                      std::vector<lsp::CodeAction>& actions);

void TryAddBoolConversionFix(const CodeActionRequest& request, TSNode rootNode,
                             std::vector<lsp::CodeAction>& actions);

void TryAddGenerateFuncdefFix(const CodeActionRequest& request, TSNode rootNode,
                              std::vector<lsp::CodeAction>& actions);

void TryAddSortAndCleanIncludesAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions);

} // namespace angel_lsp::features
