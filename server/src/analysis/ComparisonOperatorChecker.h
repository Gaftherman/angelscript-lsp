#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Checks if a type defines an opImplConv to the target type.
 * @param[in] typeName Source type name.
 * @param[in] targetType Target type name.
 * @param[in] table Symbol table for hierarchy lookup.
 * @return True if implicit conversion operator exists.
 */
bool TypeHasOpImplConvTo(const std::string& typeName, const std::string& targetType, const SymbolTable& table);

/**
 * @brief Validates operand type compatibility for binary comparison operators.
 * @param[in] node Syntax tree node of a binary_expression.
 * @param[in] scope Lexical scope enclosing the binary expression.
 * @param[in,out] ctx Diagnostic collection context.
 */
void CheckComparisonOperatorCompatibility(TSNode node, const Scope* scope, DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
