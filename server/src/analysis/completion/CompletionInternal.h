#pragma once

#include "analysis/completion/CompletionTypes.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief True when a member is its class's constructor or destructor.
 * @param[in] sym Candidate symbol.
 * @param[in] typeName Container type name.
 * @return True if symbol is a constructor or destructor.
 */
bool IsConstructorOrDestructor(const analysis::Symbol& sym, const std::string& typeName);

/**
 * @brief Formats function signature with substituted template arguments.
 * @param[in] sym Function symbol.
 * @param[in] binding Deduced template bindings.
 * @param[in] templateArgs Explicit template argument names.
 * @return Formatted declaration string.
 */
std::string FormatMethodDetail(const analysis::Symbol& sym, const analysis::TemplateBinding& binding,
                               const std::vector<std::string>& templateArgs);

/**
 * @brief Collects the class and interface inheritance hierarchy using flat worklist traversal.
 * @param[in] symbolTable Global symbol table.
 * @param[in] initialTypeName Starting type name.
 * @return Vector of type names in the hierarchy.
 */
std::vector<std::string> GetInheritedTypeHierarchy(const analysis::SymbolTable& symbolTable,
                                                   const std::string& initialTypeName);

/**
 * @brief Extracts the prefix substring of a line up to the cursor character.
 * @param[in] sourceCode Entire document string.
 * @param[in] line 0-indexed line number.
 * @param[in] character 0-indexed column character.
 * @return Substring preceding cursor on that line.
 */
std::string GetLinePrefix(std::string_view sourceCode, uint32_t line, uint32_t character);

/**
 * @brief Formats sort text prefix for a symbol proximity tier.
 * @param[in] prox Symbol proximity tier.
 * @param[in] label Symbol display label.
 * @return Formatted sort text string.
 */
std::string FormatProximitySortText(SymbolProximity prox, std::string_view label);

/**
 * @brief Determines proximity tier of a symbol relative to current request URI.
 * @param[in] sym Target symbol.
 * @param[in] currentUri URI of currently active document.
 * @param[in] predefinedExt Predefined file extension.
 * @return Computed SymbolProximity.
 */
SymbolProximity DetermineSymbolProximity(const analysis::Symbol& sym, std::string_view currentUri,
                                         std::string_view predefinedExt);

/**
 * @brief Retrieves configured predefined file extension from completion request.
 * @param[in] request Completion request.
 * @return Predefined file extension string view.
 */
std::string_view GetPredefinedExtension(const CompletionQuery& request);

/**
 * @brief Adds a completion item to collector if its label has not yet been offered.
 * @param[in,out] collector Completion collector context.
 * @param[in] candidate Completion item candidate attributes.
 */
void AddItemIfNew(CompletionCollector& collector, CompletionCandidate candidate);

/**
 * @brief Builds snippet string for function call placeholders.
 * @param[in] name Function name.
 * @param[in] params Function parameter list.
 * @return Formatted snippet string.
 */
std::string CallSnippet(const std::string& name, const std::vector<analysis::ParameterInformation>& params);

/**
 * @brief Static list of AngelScript VM primitive type names.
 * @return Vector of primitive strings.
 */
std::vector<std::string> GetPrimitiveTypeNames();

/**
 * @brief Constructs template insert snippet for a class symbol.
 * @param[in] sym Candidate class symbol.
 * @return Formatted snippet with placeholders, or empty string.
 */
std::string TemplateInsertSnippet(const analysis::Symbol& sym);

/**
 * @brief Classifies the lexical context at a specific byte offset.
 * @param[in] source Source code view.
 * @param[in] offset Target byte offset.
 * @return LexicalContext category.
 */
LexicalContext ContextAtOffset(std::string_view source, size_t offset);

/**
 * @brief Checks if prefix terminates at a `case` or `default` label colon.
 * @param[in] prefix Prefix string.
 * @return True if after case label colon.
 */
bool IsAfterCaseLabelColon(const std::string& prefix);

/**
 * @brief Checks if prefix places cursor inside an unclosed template bracket `<...>`.
 * @param[in] prefix Prefix string view.
 * @return True if cursor is inside template argument brackets.
 */
bool IsInsideTemplateArguments(std::string_view prefix);

/**
 * @brief Generates call snippet for an unambiguous function name.
 * @param[in] name Function name.
 * @param[in] table Global symbol table.
 * @return Call snippet string, or empty.
 */
std::string CallSnippetForName(const std::string& name, const analysis::SymbolTable& table);

/**
 * @brief Generates template insert snippet for a named template class.
 * @param[in] name Class name.
 * @param[in] table Global symbol table.
 * @return Snippet string, or empty.
 */
std::string TemplateSnippetForName(const std::string& name, const analysis::SymbolTable& table);

/**
 * @brief Returns list of AngelScript reserved and contextual keywords.
 * @return Reference to static keyword vector.
 */
const std::vector<std::string>& GetKeywords();

/**
 * @brief Handles early return for include paths or suppressed lexical contexts.
 * @param[in] request Completion request.
 * @param[in] prefix Prefix text before cursor.
 * @return Optional vector of completion items (may be empty if suppressed).
 */
std::optional<std::vector<CompletionEntry>> HandleIncludeOrLexicalSuppression(const CompletionQuery& request,
                                                                              const std::string& prefix);

/**
 * @brief Converts a line and column to a byte offset in the document text.
 * @param[in] sourceCode Document text.
 * @param[in] line 0-based line.
 * @param[in] character 0-based column, counted as bytes the way a Tree-Sitter point counts it.
 * @return Byte offset of the position; the text size when the line is past the end.
 */
uint32_t ByteOffsetAt(std::string_view sourceCode, uint32_t line, uint32_t character);

/**
 * @brief Reconstructs the qualified namespace path for a lexical scope node.
 * @param[in] scope Starting lexical scope node.
 * @param[in] request Completion request context.
 * @return Qualified namespace path, or empty string if at global scope.
 */
std::string BuildScopeNamespacePath(const Scope* scope, const CompletionQuery& request);

/**
 * @brief Collects all namespaces visible at the cursor via enclosing blocks and `using namespace` directives.
 * @param[in] request Completion request context.
 * @param[in] innermostScope Innermost lexical scope at cursor, if available.
 * @return Ordered list of unique visible namespace names.
 */
std::vector<std::string> CollectVisibleNamespaces(const CompletionQuery& request,
                                                  const analysis::Scope* innermostScope);

/**
 * @brief Attempts to complete members following a scope qualifier `Qualifier::`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if scope resolution completed.
 */
bool TryCompleteScopeResolution(const std::string& prefix, CompletionCollector& collector);

/**
 * @brief Completes type names when cursor is inside template argument brackets `<...>`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if template arguments completed.
 */
bool TryCompleteTemplateArguments(const std::string& prefix, CompletionCollector& collector);

/**
 * @brief Retrieves backward declaration text before cursor up to opening brace or semicolon.
 */
std::string GetBackwardDeclarationText(std::string_view sourceCode, uint32_t line, uint32_t character);

/**
 * @brief Attempts to complete strictly allowed inheritance base classes, interfaces, and namespaces.
 */
bool TryCompleteInheritance(const std::string& prefix, CompletionCollector& collector);

/**
 * @brief Resolves base container type name, handling unqualified template class lookups.
 * @param[in] canonicalType Canonical type name.
 * @param[in] symbolTable Global symbol table.
 * @return Fully qualified base container name.
 */
std::string FindCanonicalBaseContainer(const std::string& canonicalType, const analysis::SymbolTable& symbolTable);

/**
 * @brief Substitutes template type parameters in a type signature.
 * @param[in] text Input type string.
 * @param[in] binding Template bindings.
 * @param[in] templateArgs Explicit template arguments.
 * @return Substituted type string.
 */
std::string SubstituteTypeParameters(std::string text, const analysis::TemplateBinding& binding,
                                     const std::vector<std::string>& templateArgs);

/**
 * @brief Checks if an open parenthesis immediately follows the cursor position.
 * @param[in] sourceCode Document source text.
 * @param[in] position Cursor position.
 * @return True if '(' follows cursor ignoring inline whitespace, false otherwise.
 */
bool HasTrailingOpenParen(std::string_view sourceCode, const TargetPosition& position);

/**
 * @brief Determines whether function/method call parentheses snippet should be generated.
 * @param[in] collector Completion collector context.
 * @return True if snippet parentheses should be inserted, false otherwise.
 */
bool ShouldCompleteParens(const CompletionCollector& collector);

/**
 * @brief Resolves name of the class enclosing cursor position.
 * @param[in] request Completion request.
 * @return Enclosing class name, or empty string.
 */
std::string FindEnclosingClassName(const CompletionQuery& request);

/**
 * @brief Checks if a dot is part of a numeric literal like '3.'.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] dotPos Index of the dot character.
 * @return True if dot is part of a number literal.
 */
bool IsNumericLiteralDot(std::string_view prefix, size_t dotPos);

/**
 * @brief Resolves a receiver written as a plain name: `this`, a local or global variable, a member of the
 *        enclosing class, or a global `get_` accessor.
 * @param[in] nodeText Receiver spelling; a leading `this.` is stripped and resolved recursively.
 * @param[in] request Completion query supplying the symbol table and cursor.
 * @param[in] innermostScope Innermost lexical scope at the cursor, or null.
 * @return Raw type name, or an empty string when the name resolves to nothing.
 */
std::string ResolveNamedReceiverType(std::string_view nodeText, const CompletionQuery& request,
                                     const analysis::Scope* innermostScope);

/**
 * @brief Peels index bracket sequences preceding a dot access, capturing the last inner index argument.
 * @param[in] prefix Source prefix preceding cursor.
 * @param[in,out] s Offset position before bracket sequence.
 * @return Struct with number of index brackets peeled and last index argument string.
 */
IndexPeelResult PeelIndexBracketsWithExpr(const std::string& prefix, size_t& s);

/**
 * @brief Skips spaces and horizontal tabs backward from a given position.
 * @param[in] text Input string containing the source line or expression.
 * @param[in] pos Starting character offset.
 * @return Adjusted offset positioned at the first non-whitespace character or zero.
 */
size_t SkipInlineWhitespaceBackward(const std::string& text, size_t pos);

/**
 * @brief Finds the start byte offset of an identifier moving backwards from the specified end position.
 * @param[in] text Source code text buffer.
 * @param[in] end Byte offset where backward search begins.
 * @return Starting byte offset of the identifier.
 */
size_t ExtractIdentifierBackward(const std::string& text, size_t end);

/**
 * @brief Deduces basic literal or identifier type from an index expression string.
 * @param[in] expr Raw trimmed expression inside index brackets.
 * @param[in] request Completion request context.
 * @param[in] scope Innermost lexical scope at cursor.
 * @return Deduced type name string, or empty string.
 */
std::string DeduceLiteralOrNamedType(std::string_view expr, const CompletionQuery& request,
                                     const analysis::Scope* scope);

/**
 * @brief Checks if an identifier denotes an indexed property accessor (getter with parameters).
 * @param[in] table Symbol table.
 * @param[in] parentType Enclosing class/type name, or empty for global.
 * @param[in] name Property name without prefix.
 * @param[in] enclosingClass Enclosing class name in current scope, if any.
 * @return True if an indexed getter accessor exists.
 */
bool IsIndexedPropertyAccessor(const analysis::SymbolTable& table, const std::string& parentType, std::string_view name,
                               std::string_view enclosingClass = "");

/**
 * @brief Finds the member-access dot before the identifier being typed at the end of a line prefix.
 * @param[in] prefix Line text before the cursor.
 * @return Column of the `.`, or std::nullopt when there is none or it belongs to a number literal.
 */
std::optional<size_t> FindAccessDotCol(const std::string& prefix);

/**
 * @brief Offers the members of a type and of every type it inherits from, with template arguments bound.
 * @param[in] rawTypeName Receiver type as resolved, before array canonicalization.
 * @param[in] memberPrefix Part of the member name already typed after the dot.
 * @param[in,out] collector Completion collector receiving the members.
 */
void PopulateHierarchicalMembers(const std::string& rawTypeName, std::string_view memberPrefix,
                                 CompletionCollector& collector);

/**
 * @brief Attempts to complete member expression following a `.`.
 * @param[in] prefix Prefix text before cursor.
 * @param[in] innermostScope Lexical scope at cursor.
 * @param[in,out] collector Completion collector context.
 * @return True if member completion completed.
 */
bool TryCompleteMemberAccess(const std::string& prefix, const analysis::Scope* innermostScope,
                             CompletionCollector& collector);

/**
 * @brief Adds an enum constant candidate, qualifying with enum type name when enabled.
 * @param[in,out] collector Completion collector context.
 * @param[in] sym Target enum constant symbol.
 */
void AddEnumConstantCandidate(CompletionCollector& collector, const analysis::Symbol& sym);

/**
 * @brief Collects local variable and parameter definitions from innermost scope outward.
 * @param[in] innermostScope Lexical scope at cursor.
 * @param[in,out] collector Completion collector context.
 */
void CollectScopeDefinitions(const analysis::Scope* innermostScope, CompletionCollector& collector);

/**
 * @brief Collects member methods and fields of the enclosing class.
 * @param[in,out] collector Completion collector context.
 */
void CollectEnclosingClassMembers(CompletionCollector& collector);

/**
 * @brief Extracts the trailing identifier query prefix from a line prefix.
 * @param[in] linePrefix Line text up to the cursor position.
 * @return Extracted identifier query prefix, or empty if none.
 */
std::string_view ExtractQueryPrefix(std::string_view linePrefix);

/**
 * @brief Collects all global scope symbols from the symbol table.
 * @param[in] accessorsAreProperties Whether property accessors are exposed.
 * @param[in] accessorKeywordRequired Whether property keyword is required.
 * @param[in] queryPrefix Prefix to filter symbol table buckets by.
 * @param[in,out] collector Completion collector context.
 */
void CollectGlobalSymbols(bool accessorsAreProperties, bool accessorKeywordRequired, std::string_view queryPrefix,
                          CompletionCollector& collector);

/**
 * @brief Collects symbols from enclosing namespaces and active `using namespace` directives.
 * @param[in] innermostScope Innermost lexical scope at cursor.
 * @param[in] queryPrefix Prefix to filter candidate symbol names by.
 * @param[in,out] collector Completion collector context.
 */
void CollectVisibleNamespaceSymbols(const analysis::Scope* innermostScope, std::string_view queryPrefix,
                                    CompletionCollector& collector);

/**
 * @brief Offers operator-method snippets (`opCall`, `opIndex`, `opAssign`, ...) in a class body outside methods.
 * @param[in] innermostScope Innermost lexical scope at the cursor, or null.
 * @param[in,out] collector Completion collector receiving the snippets.
 */
void CollectClassOperatorSnippets(const analysis::Scope* innermostScope, CompletionCollector& collector);

/**
 * @brief Inserts statement and declaration snippet completions.
 * @param[in,out] collector Completion collector context.
 */
void CollectDeclarationSnippets(CompletionCollector& collector);

/**
 * @brief Inserts keyword completions into collector.
 * @param[in,out] collector Completion collector context.
 */
void CollectKeywords(CompletionCollector& collector);

/**
 * @brief Finds the call whose argument list encloses the cursor and the argument being typed.
 * @param[in] prefix Line text before the cursor.
 * @param[out] outCallee Callee spelling before the unclosed `(`.
 * @param[out] outArgIndex 0-based index of the argument at the cursor.
 * @return True when an unclosed call with a non-empty callee precedes the cursor.
 */
bool ExtractCallContext(std::string_view prefix, std::string& outCallee, size_t& outArgIndex);

/**
 * @brief Finds the expected parameter type of a callee function at the given argument index.
 * @param[in] callee Callee name or member expression string.
 * @param[in] argIndex Index of the target argument.
 * @param[in] request Completion request.
 * @param[in] scope Lexical scope at cursor.
 * @return Resolved parameter type name, or empty string.
 */
std::string FindCalleeParameterType(const std::string& callee, size_t argIndex, const CompletionQuery& request,
                                    const analysis::Scope* scope);

/**
 * @brief Extracts expected target type from declaration assignment before cursor.
 * @param[in] prefix Text before cursor on current line.
 * @return Extracted target type name, or empty string.
 */
std::string ExtractAssignmentTargetType(std::string_view prefix);

/**
 * @brief Applies smart type-aware ranking to completion items based on active context.
 * @param[in,out] items Completion item list to rank and sort.
 * @param[in] prefix Text before cursor on current line.
 * @param[in] scope Innermost scope at cursor.
 * @param[in] request Completion request context.
 */
void ApplySmartTypeRanking(std::vector<CompletionEntry>& items, std::string_view prefix, const analysis::Scope* scope,
                           const CompletionQuery& request);

/**
 * @brief Sorts completion items deterministically according to their sortText and label.
 * @param[in,out] items Vector of completion items to sort.
 */
void SortCompletionItemsByProximity(std::vector<CompletionEntry>& items);

} // namespace angel_lsp::analysis::completion
