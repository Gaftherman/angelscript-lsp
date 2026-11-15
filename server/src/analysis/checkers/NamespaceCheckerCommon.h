#pragma once

#include "analysis/checkers/NamespaceChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NamespaceUsageFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/SymbolTable.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Outcome of resolving a qualified scope lookup (`qualifier::member` or qualifier-only).
 */
enum class QualifiedLookupStatus : uint8_t
{
    Resolved,         ///< Qualifier and member both resolve, or lookup is deferred to member checks.
    UnknownQualifier, ///< No reachable candidate matches the qualifier as a namespace, enum, or type.
    UndefinedMember   ///< Qualifier resolved to a namespace or enum, but the requested member was not found.
};

/**
 * @brief Parameters describing a qualified lookup site.
 */
struct QualifiedLookupRequest
{
    std::string_view qualifier;
    std::string_view member;
    uint32_t startByte = 0;
    bool hasLeadingGlobal = false;
    bool checkMember = true;
    std::string_view sourceCode;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Checks if a name matches a declared member or accessor property in container members.
 * @param[in] name Member or property name to check.
 * @param[in] members Precomputed container member sets from RuleIndex.
 * @param[in] reqKeyword Whether the `property` keyword is required for accessor properties.
 * @return True if found in members or accessor properties.
 */
bool HasContainerOrAccessorMember(std::string_view name, const rules::ContainerMembers& members, bool reqKeyword);

/**
 * @brief Resolves a qualified scope/member lookup across global, enclosing namespace, and using scopes.
 * @param[in] req Lookup parameters including qualifier, member, byte offset, and index handles.
 * @param[in] ctx Active diagnostic context providing symbol table and rule index.
 * @return Resolution status indicating resolved, unknown qualifier, or undefined member.
 */
QualifiedLookupStatus ResolveQualifiedMember(const QualifiedLookupRequest& req, const DiagnosticContext& ctx);

/**
 * @brief Context bundle for validating scoped identifier prefixes.
 */
struct ScopedIdentifierCheckContext
{
    std::string_view sourceCode;
    const NodeIndex* nodeIndex = nullptr;
    const SymbolTable& table;
};

/**
 * @brief Context bundle for validating unqualified call expressions.
 */
struct NamespaceCallCheckContext
{
    std::string_view sourceCode;
    const NodeIndex* nodeIndex = nullptr;
};

/**
 * @brief Checks if a scoped identifier refers to a known namespace or type prefix.
 * @param[in] fact Scoped identifier fact to validate.
 * @param[in] sctx Context carrying source code, node index, and symbol table.
 * @param[in,out] ctx Diagnostic sink accumulating emitted findings.
 */
void CheckScopedIdentifier(const ScopedIdentifierFact& fact, const ScopedIdentifierCheckContext& sctx,
                           DiagnosticContext& ctx);

/**
 * @brief Checks if an unqualified function call resolves to an in-scope symbol.
 * @param[in] call Call site fact to validate.
 * @param[in] cctx Context carrying source code and node index.
 * @param[in,out] ctx Diagnostic sink accumulating emitted findings.
 */
void CheckCallExpression(const CallSiteFact& call, const NamespaceCallCheckContext& cctx, DiagnosticContext& ctx);

/**
 * @brief Checks if an import declaration mistakenly specifies a body block.
 * @param[in] fact Import declaration fact to validate.
 * @param[in,out] ctx Diagnostic sink accumulating emitted findings.
 */
void CheckImportBody(const ImportDeclarationFact& fact, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
