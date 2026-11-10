#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/MemberAccessFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ScopeSymbolLookup.h"

#include <string_view>

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
MemberLookupResult FindMember(std::string_view typeName, std::string_view memberName, const SymbolTable& table,
                              bool accessorKeywordRequired);

/**
 * @brief Finds a member across type hierarchy and configured engine proxy base classes.
 * @param[in] objectType Owning object type name.
 * @param[in] memberName Member name to look up.
 * @param[in] table Symbol table.
 * @param[in] ctx Diagnostic context containing optional engine rules.
 * @return MemberLookupResult structure indicating whether the member was found.
 */
MemberLookupResult FindMemberWithProxies(std::string_view objectType, std::string_view memberName,
                                         const SymbolTable& table, const DiagnosticContext& ctx);

/**
 * @brief Checks whether an identifier attempts forbidden outer variable or member capture in a closure.
 * @param[in] fact Pre-extracted member access identifier fact.
 * @param[in] scope Current lexical scope.
 * @param[in,out] ctx Diagnostic collection context.
 * @return True if an error diagnostic was emitted; false otherwise.
 */
bool CheckClosureDisallowedAccess(const MemberAccessIdentifierFact& fact, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Validates member expression syntax, privacy rules, and accessor availability.
 * @param[in] fact Pre-extracted member expression fact.
 * @param[in] scope Current lexical scope enclosing the expression.
 * @param[in,out] ctx Diagnostic context sink.
 */
void CheckMemberExpression(const MemberExpressionFact& fact, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Validates an identifier fact for implicit member access and lambda capture violations.
 * @param[in] fact Pre-extracted member access identifier fact.
 * @param[in] scope Current lexical scope enclosing the identifier.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckIdentifierFact(const MemberAccessIdentifierFact& fact, const Scope* scope, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
