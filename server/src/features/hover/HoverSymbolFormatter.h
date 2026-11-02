#pragma once

#include "features/hover/HoverHandler.h"
#include "parser/ASTUtils.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Squeezes runs of whitespace into single spaces so a declaration appears on one line.
 * @param[in] text Input string view.
 * @return Collapsed string with single spaces.
 */
std::string CollapseWhitespace(std::string_view text);

/**
 * @brief Finds the declaration node enclosing a point and reads its text back.
 * @param[in] root Tree-sitter root node.
 * @param[in] sourceCode Source text buffer.
 * @param[in] point Row and column coordinates of the declaration name.
 * @param[in] wantedSymbol AST symbol to look up.
 * @return Whitespace-collapsed declaration text, or empty string if not found.
 */
std::string ExtractDeclarationTextAt(TSNode root, const std::string& sourceCode, TSPoint point, TSSymbol wantedSymbol);

/**
 * @brief Renders the single hover line describing a symbol of any kind.
 * @param[in] sym Target symbol descriptor.
 * @param[in] symbolTable Optional workspace symbol table.
 * @return Formatted declaration string.
 */
std::string FormatDeclarationText(const analysis::Symbol& sym, const analysis::SymbolTable* symbolTable = nullptr);

/**
 * @brief Renders the hover line for a variable-like symbol, tagged with its role prefix.
 * @param[in] sym Symbol descriptor.
 * @param[in] role Prefix shown to user, e.g. "(property) ".
 * @return Formatted variable declaration string.
 */
std::string FormatVariableSignature(const analysis::Symbol& sym, const char* role);

/**
 * @brief Drops duplicate symbol declarations from the candidate collection.
 * @param[in,out] symbols Candidate symbol vector to deduplicate.
 */
void RemoveDuplicateSymbols(std::vector<analysis::Symbol>& symbols);

/**
 * @brief Resolves enclosing call node from a candidate AST node.
 * @param[in] node Starting syntax node.
 * @return Enclosing call or construct call node, or null node.
 */
TSNode FindEnclosingCallNode(TSNode node);

/**
 * @brief Resolves best matching call overload candidate against call argument types.
 * @param[in] node Callee AST node.
 * @param[in] candidates Candidate symbol collection.
 * @param[in] request Hover request context.
 * @param[in] scope Enclosing lexical scope, or nullptr.
 * @return Best matching symbol if resolved, std::nullopt otherwise.
 */
std::optional<analysis::Symbol> ResolveCallOverload(TSNode node, const std::vector<analysis::Symbol>& candidates,
                                                    const HoverRequest& request, const analysis::Scope* scope);

/**
 * @brief Resolves best matching call overload, or finds declaration matching cursor line.
 * @param[in,out] symbols Candidate symbol list.
 * @param[in] ctx Hover query context.
 * @return Best matching symbol if resolved or matched at declaration, std::nullopt otherwise.
 */
std::optional<analysis::Symbol> ResolveOrMatchCandidate(std::vector<analysis::Symbol>& symbols,
                                                        const HoverQueryContext& ctx);

/**
 * @brief Formats synthesized property header line if property accessor.
 * @param[in] symbols Candidate symbol list.
 * @param[in] accessorPropertyType Synthesized property accessor type.
 * @param[in] nodeText Syntax node text.
 * @return Formatted property header line, or empty string.
 */
std::string FormatSynthesizedPropertyHeader(const std::vector<analysis::Symbol>& symbols,
                                            std::string_view accessorPropertyType, std::string_view nodeText);

/**
 * @brief Formats hover information for a resolved set of symbols with signature and overload summary.
 * @param[in,out] symbols Candidate symbol list.
 * @param[in] accessorPropertyType Optional synthesized property accessor type.
 * @param[in] ctx Hover query context.
 * @return Formatted LSP hover response.
 */
lsp::Hover FormatSymbolsHover(std::vector<analysis::Symbol>& symbols, std::string_view accessorPropertyType,
                              const HoverQueryContext& ctx);

/**
 * @brief Formats hover information for class member access.
 * @param[in,out] memberSymbols Candidate member symbols list.
 * @param[in] accessorPropertyType Optional synthesized property accessor type.
 * @param[in] ctx Hover query context.
 * @return Formatted LSP hover response.
 */
std::optional<lsp::Hover> FormatMemberHover(std::vector<analysis::Symbol>& memberSymbols,
                                            std::string_view accessorPropertyType, const HoverQueryContext& ctx);

/**
 * @brief Formats parameter hover text from an AST LocalDefinition.
 * @param[in] def Local definition descriptor.
 * @param[in] typeName Deduced type name string view.
 * @param[in] ctx Hover query context.
 * @param[in,out] md Destination markdown string.
 */
void FormatParameterHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                          std::string& md);

/**
 * @brief Formats variable hover text from an AST LocalDefinition.
 * @param[in] def Local definition descriptor.
 * @param[in] typeName Deduced type name string view.
 * @param[in] ctx Hover query context.
 * @param[in,out] md Destination markdown string.
 */
void FormatVariableHover(const analysis::LocalDefinition& def, std::string_view typeName, const HoverQueryContext& ctx,
                         std::string& md);

/**
 * @brief Formats named argument parameter hover from candidate functions.
 * @param[in] candidates Callee candidate symbols.
 * @param[in] ctx Hover query context.
 * @return Formatted hover response if matched, std::nullopt otherwise.
 */
std::optional<lsp::Hover> FormatNamedArgHover(const std::vector<analysis::Symbol>& candidates,
                                              const HoverQueryContext& ctx);

/**
 * @brief Checks if a node corresponds to a direct-initialization variable declarator.
 * @param[in] node AST node.
 * @return True if declarator with argument list, false otherwise.
 */
bool IsDirectInitDeclarator(TSNode node);

/**
 * @brief Resolves constructor candidate symbol for a direct-init variable declarator.
 * @param[in] declarator Declarator TSNode.
 * @param[in] typeName Declared variable type name.
 * @param[in] ctx Hover query context.
 * @return Resolved constructor symbol if matched, std::nullopt otherwise.
 */
std::optional<analysis::Symbol> ResolveConstructorForDeclarator(TSNode declarator, std::string_view typeName,
                                                                const HoverQueryContext& ctx);

/**
 * @brief Appends constructor signature to hover markdown if declarator invokes a constructor.
 * @param[in] node AST declarator node.
 * @param[in] typeName Base type name.
 * @param[in] ctx Hover query context.
 * @param[in,out] md Output markdown string.
 */
void TryAppendConstructorSignature(TSNode node, std::string_view typeName, const HoverQueryContext& ctx,
                                   std::string& md);

/**
 * @brief Disambiguates between Class and Namespace symbols sharing the same identifier.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidate symbol collection to filter.
 */
void DisambiguateContainerSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief Filters candidate symbols when used in a type context to prefer types over constructors/methods.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidate symbol collection to filter.
 */
void DisambiguateTypeContextSymbols(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief Resolves constructor symbols when hovering over a class name used as a call target.
 * @param[in] ctx Hover query context.
 * @param[in,out] symbols Candidate symbol collection.
 */
void AppendConstructorSymbolsIfCall(const HoverQueryContext& ctx, std::vector<analysis::Symbol>& symbols);

/**
 * @brief Filters out non-root symbols when explicitly qualified with leading global scope ::.
 * @param[in,out] symbols Candidate symbol collection.
 * @param[in] gsq Global scope qualifier descriptor.
 */
void FilterDirectRootGlobalSymbols(std::vector<analysis::Symbol>& symbols, const parser::GlobalScopeQualifier& gsq);

/**
 * @brief Determines if an AST node is positioned in a type specifier or using namespace context.
 * @param[in] node AST node to inspect.
 * @return Context preference indicating class, namespace, or neutral.
 */
ContainerContextPreference CheckContextPreference(TSNode node);

/**
 * @brief Attempts hover resolution for expressions or local definitions (auto, member access, locals, parameters, named
 *        args).
 * @param[in] ctx Hover query context.
 * @return Populated Hover if node is an expression or local definition, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverExpressionOrLocal(const HoverQueryContext& ctx);

/**
 * @brief Attempts hover resolution for candidate symbols in scope or global hierarchy.
 * @param[in,out] ctx Hover query context.
 * @return Populated Hover if candidate symbol matches, std::nullopt otherwise.
 */
std::optional<lsp::Hover> TryHoverSymbolCandidates(HoverQueryContext& ctx);
} // namespace angel_lsp::features
