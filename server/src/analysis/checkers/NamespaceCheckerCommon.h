#pragma once

#include "analysis/checkers/NamespaceChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NamespaceUsageFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolTable.h"

#include <span>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

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
