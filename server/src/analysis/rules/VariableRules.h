#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

namespace angel_lsp::analysis
{
struct Scope;
class SymbolTable;
} // namespace angel_lsp::analysis

namespace angel_lsp::analysis::rules
{
/**
 * @brief Validates a variable, field or virtual property declaration.
 *
 * Covers what the declared type allows - void, handles on primitives, funcdefs that must be
 * handles, mixins used as a type - and what the declaration's modifiers allow where it sits.
 * Virtual property accessors are validated here too, since they arrive as VariableSignature.
 *
 * @param sym Variable or property symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void ValidateVariable(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Emits as-err-null-non-handle for every module-scope global or class-body field in
 *        the current document whose declared type is a non-handle primitive initialized to null.
 * @param[in] symbolTable Symbol table to inspect.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckNullAssignedToNonHandle(const SymbolTable& symbolTable, DiagnosticContext& ctx);

/**
 * @brief Emits as-err-null-non-handle for local variables in function scopes.
 * @param[in] rootScope Root lexical scope to inspect.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckNullAssignedToNonHandleInScope(const Scope* rootScope, DiagnosticContext& ctx);

/**
 * @brief Validates local variable declarations across the scope tree.
 * @param[in] rootScope Root lexical scope to inspect.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckLocalVariableDeclarations(const Scope* rootScope, DiagnosticContext& ctx);

/**
 * @brief Validates names declared within a local scope (e.g., duplicates, reserved keywords).
 * @param[in] scope Lexical scope being inspected.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckLocalNames(const Scope* scope, DiagnosticContext& ctx);

/**
 * @brief Validates types used by local variables within a scope.
 * @param[in] scope Lexical scope being inspected.
 * @param[in,out] ctx Diagnostic context accumulating emitted findings.
 */
void CheckLocalTypes(const Scope* scope, DiagnosticContext& ctx);

} // namespace angel_lsp::analysis::rules
