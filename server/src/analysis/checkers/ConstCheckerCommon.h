#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/ConstChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ConstSiteFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves whether a target expression evaluates to a const or mutable value.
 * @param[in] target Pre-extracted assignment target fact.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveConstness(const ConstAssignmentTargetFact& target, const ConstSiteFacts& facts,
                           const ExpressionTypeContext& ctx);

/**
 * @brief Resolves whether a method call receiver evaluates to a const or mutable value.
 * @param[in] call Pre-extracted method call fact.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveConstness(const ConstMethodCallFact& call, const ConstSiteFacts& facts,
                           const ExpressionTypeContext& ctx);

/**
 * @brief Resolves constness for a handle receiver expression.
 * @param[in] name Identifier name of the handle.
 * @param[in] startByte Start byte offset of the handle usage.
 * @param[in] facts Document-wide const site facts.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveHandleConstness(std::string_view name, uint32_t startByte, const ConstSiteFacts& facts,
                                 const ExpressionTypeContext& ctx);

/**
 * @brief Checks assignment expressions for mutations of const variables or read-only references.
 * @param[in] assignment Pre-extracted assignment fact.
 * @param[in] facts Document-wide const site facts.
 * @param[in] scope Lexical scope active at assignment location.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckAssignment(const ConstAssignmentFact& assignment, const ConstSiteFacts& facts, const Scope* scope,
                     DiagnosticContext& ctx);

/**
 * @brief Validates method calls on const receivers to ensure a const overload is available.
 * @param[in] call Pre-extracted method call fact.
 * @param[in] facts Document-wide const site facts.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckMethodCall(const ConstMethodCallFact& call, const ConstSiteFacts& facts, const Scope* scope,
                     DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
