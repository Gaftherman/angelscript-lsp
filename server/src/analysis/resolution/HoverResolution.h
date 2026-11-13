#pragma once

#include "analysis/documentation/HoverMarkdown.h"
#include "analysis/index/HoverFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "config/ServerConfig.h"

#include <span>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the best matching function overload for a call site under the cursor.
 * @param[in] startByte Inclusive start byte offset of the target node.
 * @param[in] endByte Exclusive end byte offset of the target node.
 * @param[in] candidates Candidate overload symbols to rank.
 * @param[in] ctx Semantic expression context containing symbol table and document text.
 * @return Best matching symbol if resolution succeeds; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<Symbol> ResolveCallOverload(uint32_t startByte, uint32_t endByte,
                                                        std::span<const Symbol> candidates,
                                                        const ExpressionTypeContext& ctx);

/**
 * @brief Resolves the matching constructor symbol for a variable declarator with arguments.
 * @param[in] declStartByte Inclusive start byte offset of declarator or child identifier.
 * @param[in] declEndByte Exclusive end byte offset of declarator or child identifier.
 * @param[in] typeName Declared type name whose constructors are resolved.
 * @param[in] ctx Semantic expression context.
 * @return Resolved constructor symbol if matched; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<Symbol> ResolveConstructorForDeclarator(uint32_t declStartByte, uint32_t declEndByte,
                                                                    std::string_view typeName,
                                                                    const ExpressionTypeContext& ctx);

/**
 * @brief Formats constructor signature text for a variable declarator with argument list.
 * @param[in] nodeStartByte Inclusive start byte offset of declarator or child identifier.
 * @param[in] nodeEndByte Exclusive end byte offset of declarator or child identifier.
 * @param[in] typeName Declared variable type name.
 * @param[in] ctx Semantic expression context.
 * @return Formatted constructor declaration signature, or empty string if not matched.
 */
[[nodiscard]] std::string ResolveConstructorSignature(uint32_t nodeStartByte, uint32_t nodeEndByte,
                                                      std::string_view typeName, const ExpressionTypeContext& ctx);

/**
 * @brief Resolves the qualified receiver type name for an object expression node.
 * @param[in] objectRange Byte range of the receiver object expression.
 * @param[in] ctx Semantic expression context.
 * @param[in] virtualHostClass Host class name when inside a virtual mixin; empty otherwise.
 * @param[in] config Server configuration supplying engine rules and the array type name; may be null.
 * @return Fully qualified receiver type name, or empty string if unresolved.
 */
[[nodiscard]] std::string ResolveReceiverTypeName(IndexedByteRange objectRange, const ExpressionTypeContext& ctx,
                                                  std::string_view virtualHostClass,
                                                  const config::ServerConfig* config);

/**
 * @brief Collects candidate member symbols across the inherited type hierarchy of a receiver type.
 * @param[in] receiverTypeName Qualified receiver container type name.
 * @param[in] memberName Target member identifier name.
 * @param[in] symbolTable Workspace symbol table.
 * @return Deduplicated list of member symbols with virtual override filtering applied.
 */
[[nodiscard]] std::vector<Symbol> CollectReceiverMemberSymbols(std::string_view receiverTypeName,
                                                               std::string_view memberName,
                                                               const SymbolTable& symbolTable);

/**
 * @brief Resolves candidate callee function symbols for a call expression enclosing a named argument label.
 * @param[in] callRange Byte range of the enclosing call expression.
 * @param[in] ctx Semantic expression context.
 * @param[in] virtualHostClass Virtual mixin host class name; empty otherwise.
 * @param[in] config Server configuration forwarded to receiver resolution; may be null.
 * @return Candidate callee symbols matching the call target.
 */
[[nodiscard]] std::vector<Symbol> ResolveNamedArgCalleeCandidates(IndexedByteRange callRange,
                                                                  const ExpressionTypeContext& ctx,
                                                                  std::string_view virtualHostClass,
                                                                  const config::ServerConfig* config);

/**
 * @brief Extracts type text from the nearest ancestor parameter or variable declaration in the AST.
 * @param[in] nodeStartByte Inclusive start byte offset of target node.
 * @param[in] nodeEndByte Exclusive end byte offset of target node.
 * @param[in] ctx Semantic expression context.
 * @return Extracted type text view as owned string, or empty string if absent.
 */
[[nodiscard]] std::string InferTypeFromAst(uint32_t nodeStartByte, uint32_t nodeEndByte,
                                           const ExpressionTypeContext& ctx);

/**
 * @brief Resolves the default value of an identifier by querying local scope, visible symbols, and globals.
 * @param[in] name Identifier symbol name.
 * @param[in] nodeStartByte Inclusive start byte offset of identifier node.
 * @param[in] ctx Semantic expression context.
 * @return Unquoted default value text if resolved; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<std::string> ResolveIdentifierDefaultValue(std::string_view name, uint32_t nodeStartByte,
                                                                       const ExpressionTypeContext& ctx);

/**
 * @brief Statically evaluates a constant string literal or binary string concatenation expression.
 * @param[in] nodeStartByte Inclusive start byte offset of expression node.
 * @param[in] nodeEndByte Exclusive end byte offset of expression node.
 * @param[in] ctx Semantic expression context.
 * @return Evaluated constant string if all components resolve; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<std::string> EvaluateConstantStringExpr(uint32_t nodeStartByte, uint32_t nodeEndByte,
                                                                    const ExpressionTypeContext& ctx);

/**
 * @brief Formats anonymous lambda parameter list text from AST descendant nodes.
 * @param[in] lambdaStartByte Inclusive start byte offset of lambda expression.
 * @param[in] lambdaEndByte Exclusive end byte offset of lambda expression.
 * @param[in] ctx Semantic expression context.
 * @return Formatted lambda signature string.
 */
[[nodiscard]] std::string FormatLambdaSignature(uint32_t lambdaStartByte, uint32_t lambdaEndByte,
                                                const ExpressionTypeContext& ctx);

/**
 * @brief Deduces hover display type for a local definition, resolving auto types and parameter types.
 * @param[in] def Resolved local definition record.
 * @param[in] hoverNode Byte range of the hovered node, used for AST and lambda-parameter inference.
 * @param[in] ctx Semantic expression context with the document root, used for resolution and node recovery.
 * @return Deduced type name string, falling back to definition type.
 */
[[nodiscard]] std::string DeduceLocalDefinitionHoverType(const LocalDefinition& def, IndexedByteRange hoverNode,
                                                         const ExpressionTypeContext& ctx);

/**
 * @brief Deduces type of a variable declarator initialized with auto type specifier.
 * @param[in] declaratorRange Byte range of the variable declarator.
 * @param[in] ctx Semantic expression context.
 * @return Deduced concrete type name, or empty string if unresolved.
 */
[[nodiscard]] std::string DeduceAutoDeclaratorType(IndexedByteRange declaratorRange, const ExpressionTypeContext& ctx);

/**
 * @brief Appends visible methods from enclosing class and interface hierarchies to candidate symbol vector.
 * @param[in] targetRange Byte range of the hovered identifier, recovered to find its enclosing containers.
 * @param[in] nodeText Member identifier text to search.
 * @param[in] ctx Semantic expression context; its node index, when set, answers the container lookup.
 * @param[in,out] symbols Vector collecting resolved method symbols.
 */
void AppendEnclosingClassMethods(IndexedByteRange targetRange, std::string_view nodeText,
                                 const ExpressionTypeContext& ctx, std::vector<Symbol>& symbols);

/**
 * @brief Checks if a named member belongs to a class or interface hierarchy.
 * @param[in] classSym Container class symbol.
 * @param[in] memberName Target member name.
 * @param[in] table Symbol table containing class hierarchies.
 * @return True if member resides in class or base types; false otherwise.
 */
[[nodiscard]] bool MemberBelongsToClass(const Symbol& classSym, std::string_view memberName, const SymbolTable& table);

/**
 * @brief Checks if a named member belongs to a namespace symbol.
 * @param[in] nsSym Namespace symbol.
 * @param[in] memberName Target member name.
 * @param[in] table Symbol table containing namespace members.
 * @return True if member resides in namespace; false otherwise.
 */
[[nodiscard]] bool MemberBelongsToNamespace(const Symbol& nsSym, std::string_view memberName, const SymbolTable& table);

/**
 * @brief Hover candidate symbols plus the property type synthesized when they came from `get_`/`set_` accessors.
 * @note Member access, virtual-host and global-accessor lookups all fill it; the type stays empty for plain symbols.
 */
struct HoverCandidateSymbols
{
    std::vector<Symbol> symbols;      ///< Candidate member or accessor symbols.
    std::string accessorPropertyType; ///< Synthesized accessor property type, if resolved from get_/set_.
};

/**
 * @brief Resolves candidate member symbols and accessor property types for a member access expression.
 * @param[in] receiverRange Byte range of the receiver object expression.
 * @param[in] memberName Target member identifier name.
 * @param[in] exprCtx Semantic expression context for receiver type deduction.
 * @param[in] virtualHostClass Virtual mixin host class name; empty otherwise.
 * @param[in] config Optional server configuration supplying property accessor modes and engine rules.
 * @return Resolved member access result if candidates match; std::nullopt otherwise.
 */
[[nodiscard]] std::optional<HoverCandidateSymbols>
ResolveMemberAccess(IndexedByteRange receiverRange, std::string_view memberName, const ExpressionTypeContext& exprCtx,
                    std::string_view virtualHostClass, const config::ServerConfig* config);

/**
 * @brief Resolves deduced type for an auto variable declaration and renders the hover markdown.
 * @param[in] autoDeclRange Byte range of the enclosing variable declaration or foreach statement.
 * @param[in] isAutoHandle True if the auto keyword specifies handle (@).
 * @param[in] exprCtx Semantic expression context.
 * @return Formatted hover markdown block, or empty string if unresolved.
 */
[[nodiscard]] std::string ResolveAutoHoverMarkdown(IndexedByteRange autoDeclRange, bool isAutoHandle,
                                                   const ExpressionTypeContext& exprCtx);

/**
 * @brief Renders the complete hover markdown block for an anonymous function or lambda expression.
 * @param[in] lambdaRange Byte range of the enclosing lambda expression.
 * @param[in] exprCtx Semantic expression context for signature deduction.
 * @param[in] docCtx Document lookup context for companion doc comments.
 * @return Formatted hover markdown text.
 */
[[nodiscard]] std::string FormatLambdaHover(IndexedByteRange lambdaRange, const ExpressionTypeContext& exprCtx,
                                            const HoverDocLookupContext& docCtx);

/**
 * @brief Context parameters for querying candidate symbols in lexical and container scopes.
 */
struct ScopedSymbolQuery
{
    const IndexedHoverSite& site;         ///< AST-free hover site facts.
    std::string_view nodeText{};          ///< Target identifier name.
    std::string_view sourceCode{};        ///< Full document source text.
    const SymbolTable& symbolTable;       ///< Workspace symbol table.
    const NodeIndex* nodeIndex = nullptr; ///< Optional node index for active imports.
};

/**
 * @brief Context parameters for resolving virtual host class symbols in mixin declarations.
 */
struct VirtualHostSymbolQuery
{
    IndexedByteRange targetRange{};       ///< Byte range of target symbol.
    std::string_view nodeText{};          ///< Member identifier text.
    std::string_view virtualHostClass{};  ///< Host class name from virtual URI context.
    std::string_view sourceCode{};        ///< Document source text.
    std::string_view fileUri{};           ///< Document file URI.
    const NodeIndex* nodeIndex = nullptr; ///< Document node index.
    const SymbolTable& symbolTable;       ///< Workspace symbol table.
};

/**
 * @brief Gathers in-scope candidate symbols for an identifier or scoped member hover site.
 * @param[in] query Scoped symbol query parameters.
 * @param[out] outPathText Output view set to the scoped prefix if explicitly qualified.
 * @return Matching candidate symbols in scope.
 */
[[nodiscard]] std::vector<Symbol> CollectHoverScopedSymbols(const ScopedSymbolQuery& query,
                                                            std::string_view& outPathText);

/**
 * @brief Adds members of the mixin's host class (virtual document host, or the class including the enclosing mixin).
 * @param[in] query Virtual host symbol query parameters.
 * @param[in,out] candidates Candidate list; when the host has no direct member, its accessors are added and
 *                           `accessorPropertyType` is set from them.
 */
void AppendVirtualHostSymbols(const VirtualHostSymbolQuery& query, HoverCandidateSymbols& candidates);

/**
 * @brief Evaluates base class delegate property fallback hover markdown.
 * @param[in] query Virtual host symbol query parameters.
 * @param[in] config Optional server configuration supplying engine rules.
 * @return Formatted delegate property hover markdown, or empty string if not matched.
 */
[[nodiscard]] std::string TryHoverDelegateProperty(const VirtualHostSymbolQuery& query,
                                                   const config::ServerConfig* config);

} // namespace angel_lsp::analysis
