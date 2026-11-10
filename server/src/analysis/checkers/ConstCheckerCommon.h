#pragma once

#include "analysis/checkers/AstCheckContext.h"
#include "analysis/checkers/ConstChecker.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Identifies enclosing class and whether the enclosing method or accessor is const.
 * @param[in] node AST node inside the method.
 * @param[in] sourceCode Source text.
 * @return EnclosingMethodInfo containing class membership and const qualification.
 */
EnclosingMethodInfo FindEnclosingMethod(TSNode node, std::string_view sourceCode);

/**
 * @brief Resolves whether an AST expression evaluates to a const or mutable value.
 * @param[in] node AST expression node to inspect.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveConstness(TSNode node, const ExpressionTypeContext& ctx);

/**
 * @brief Resolves constness for a handle receiver expression.
 * @param[in] node AST node representing the handle receiver.
 * @param[in] ctx Expression type context containing symbols and source.
 * @return Resolved Constness enum value.
 */
Constness ResolveHandleConstness(TSNode node, const ExpressionTypeContext& ctx);

/**
 * @brief Checks assignment expressions for mutations of const variables or read-only references.
 * @param[in] node AST node of the assignment expression.
 * @param[in] request Contextual request bundle containing AST root and source text.
 * @param[in] scope Lexical scope active at assignment location.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckAssignment(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Validates method calls on const receivers to ensure a const overload is available.
 * @param[in] node AST node of the call expression.
 * @param[in] request Contextual request bundle containing AST root and source text.
 * @param[in] scope Lexical scope active at the call site.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckMethodCall(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
