#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ScopeSymbolLookup.h"

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Compares two type names by their last segment, so qualification cannot hide a match.
 * @param[in] a First type name.
 * @param[in] b Second type name.
 * @return True if types match or share the same terminal scope segment; false otherwise.
 */
bool IsSameAccessType(std::string_view a, std::string_view b);

/**
 * @brief Searches for a member on a type across its inherited hierarchy and accessors.
 * @param[in] typeName Name of the owning type.
 * @param[in] memberName Name of the candidate member to find.
 * @param[in] table Symbol table used for member queries.
 * @param[in] accessorKeywordRequired Whether property access requires the 'property' keyword.
 * @return Result descriptor containing access modifier, declaring class, and resolution state.
 */
MemberLookupResult FindMember(const std::string& typeName, const std::string& memberName, const SymbolTable& table,
                              bool accessorKeywordRequired);

/**
 * @brief Finds a member across type hierarchy and configured engine proxy base classes.
 * @param[in] objectType Owning object type name.
 * @param[in] memberName Member name to look up.
 * @param[in] table Symbol table.
 * @param[in] ctx Diagnostic context containing optional engine rules.
 * @return MemberLookupResult structure indicating whether the member was found.
 */
MemberLookupResult FindMemberWithProxies(const std::string& objectType, const std::string& memberName,
                                         const SymbolTable& table, const DiagnosticContext& ctx);

/**
 * @brief Identifies the innermost class body enclosing an AST node.
 * @param[in] node AST node to locate within a class body.
 * @param[in] sourceCode Document source text.
 * @param[out] insideMixin True if the enclosing class is a mixin class.
 * @param[in] nodeIndex Pre-indexed node index for span lookup.
 * @return View of the enclosing class name, or empty if node sits outside a class.
 */
std::string_view EnclosingClass(TSNode node, std::string_view sourceCode, bool& insideMixin,
                                const NodeIndex* nodeIndex);

/**
 * @brief Checks whether an identifier attempts forbidden outer variable or member capture in a closure.
 * @param[in] node AST identifier node.
 * @param[in] scope Current lexical scope.
 * @param[in] idText Identifier name.
 * @param[in,out] ctx Diagnostic collection context.
 * @return True if an error diagnostic was emitted; false otherwise.
 */
bool CheckClosureDisallowedAccess(TSNode node, const Scope* scope, std::string_view idText, DiagnosticContext& ctx);

/**
 * @brief Validates member expression syntax, privacy rules, and accessor availability.
 * @param[in] node AST member expression node.
 * @param[in] request Ast check context with document source and node index.
 * @param[in] scope Current lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic context sink.
 */
void CheckMemberExpression(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Validates an identifier node for implicit member access and lambda capture violations.
 * @param[in] node AST identifier node.
 * @param[in] request Ast check context with document source and scope root.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckIdentifierNode(TSNode node, const AstCheckContext& request, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
