#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string_view>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Checks whether AngelScript reserves the method name for an operator overload.
 * @param[in] name Unqualified name of the method.
 * @return True if the language uses that name for operator dispatch.
 */
bool IsOperatorName(std::string_view name);

/**
 * @brief Validates a method that overloads an operator.
 *        AngelScript gives the operator methods fixed shapes: opCmp returns int, opEquals returns
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
