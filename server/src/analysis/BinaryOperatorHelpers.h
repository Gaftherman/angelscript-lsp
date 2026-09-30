#pragma once

#include "analysis/DiagnosticContext.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief Cleaned operand types for binary operations.
 */
struct BinaryOperandTypes
{
    std::string left;
    std::string right;
};

/**
 * @brief Checks if a parameter definition is compatible with an argument type.
 * @param[in] param Formal parameter information.
 * @param[in] argType Actual argument type name.
 * @param[in] table Workspace symbol table.
 * @return True if parameter is compatible with argument.
 */
bool IsParameterCompatible(const ParameterInformation& param, const std::string& argType, const SymbolTable& table);

/**
 * @brief Checks if a type defines an operator overload matching the given argument type.
 * @param[in] typeName Host type name.
 * @param[in] opName Operator method name (e.g. "opOr", "opCmp").
 * @param[in] argType Argument type passed to the operator.
 * @param[in] table Workspace symbol table.
 * @return True if matching operator overload exists in hierarchy.
 */
bool TypeHasOperator(const std::string& typeName, const std::string& opName, const std::string& argType,
                     const SymbolTable& table);

/**
 * @brief Checks if a type defines an opImplConv to the target type.
 * @param[in] typeName Source type name.
 * @param[in] targetType Target type name.
 * @param[in] table Workspace symbol table.
 * @return True if implicit conversion operator exists.
 */
bool TypeHasOpImplConvTo(const std::string& typeName, const std::string& targetType, const SymbolTable& table);

/**
 * @brief Checks if a type is known to the symbol table or is a primitive/enum.
 * @param[in] type Type name to query.
 * @param[in] table Workspace symbol table.
 * @param[in] stringTypeName Effective string type name.
 * @return True if type is recognized.
 */
bool IsKnownType(const std::string& type, const SymbolTable& table, std::string_view stringTypeName);

/**
 * @brief Checks if a node or its type represents null.
 * @param[in] node AST expression node.
 * @param[in] type Resolved expression type name.
 * @return True if operand represents null.
 */
bool IsNullOperand(TSNode node, const std::string& type);

/**
 * @brief Resolves cleaned left and right operand types for a binary expression.
 * @param[in] left Left operand AST node.
 * @param[in] right Right operand AST node.
 * @param[in] scope Lexical scope of the expression.
 * @param[in] ctx Diagnostic collection context.
 * @return Cleaned operand types, or std::nullopt if unresolvable/null.
 */
std::optional<BinaryOperandTypes> ResolveCleanBinaryOperandTypes(TSNode left, TSNode right, const Scope* scope,
                                                                 const DiagnosticContext& ctx);
} // namespace angel_lsp::analysis
