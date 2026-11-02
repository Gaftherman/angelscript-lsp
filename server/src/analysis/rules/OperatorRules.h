#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

namespace angel_lsp::analysis::rules
{
/**
 * @brief Validates a method that overloads an operator.
 *
 * AngelScript gives the operator methods fixed shapes: opCmp returns int, opEquals returns
 * bool, a binary operator takes exactly one argument, opIndex takes at least one, and none of
 * them mean anything outside a class. A method whose name is not one of the operator names
 * leaves this untouched.
 *
 * @param sym Function symbol to validate.
 * @param ctx Diagnostic sink; also carries the SymbolTable every lookup goes through.
 */
void ValidateOperator(const Symbol& sym, const DiagnosticContext& ctx);

/**
 * @brief Defines expected parameter count, return type constraints, and constness for operator overloads.
 */
struct ExpectedOperatorShape
{
    TypeKind expectedReturnKind;
    std::string_view returnDiagCode;
};
} // namespace angel_lsp::analysis::rules
