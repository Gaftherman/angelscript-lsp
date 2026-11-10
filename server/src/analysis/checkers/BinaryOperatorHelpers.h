#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/types/OperandTypePair.h"

#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Tests whether an operator spelling is an equality or relational comparison.
 * @param[in] op Operator spelling.
 * @return True for `==`, `!=`, `<`, `<=`, `>` and `>=`.
 */
[[nodiscard]] constexpr bool IsComparisonOp(std::string_view op) noexcept
{
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

/**
 * @brief Tests whether an operator spelling is a relational comparison.
 * @param[in] op Operator spelling.
 * @return True for `<`, `<=`, `>` and `>=`.
 */
[[nodiscard]] constexpr bool IsRelationalOp(std::string_view op) noexcept
{
    return op == "<" || op == "<=" || op == ">" || op == ">=";
}

/**
 * @brief Tests whether an operator spelling is a bitwise or shift operator.
 * @param[in] op Operator spelling.
 * @return True for `|`, `&`, `^`, `<<`, `>>` and `>>>`.
 */
[[nodiscard]] constexpr bool IsBitwiseOp(std::string_view op) noexcept
{
    return op == "|" || op == "&" || op == "^" || op == "<<" || op == ">>" || op == ">>>";
}

/**
 * @brief Tests whether an operator spelling is an arithmetic operator.
 * @param[in] op Operator spelling.
 * @return True for `+`, `-`, `*`, `/`, `%` and `**`.
 */
[[nodiscard]] constexpr bool IsArithmeticOp(std::string_view op) noexcept
{
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%" || op == "**";
}

/**
 * @brief Tests whether a parameter accepts an argument of the given type.
 * @param[in] param Declared parameter.
 * @param[in] argType Actual argument type name.
 * @param[in] table Symbol table used to resolve conversions.
 * @return True when the argument can bind to the parameter.
 */
bool IsParameterCompatible(const ParameterInformation& param, std::string_view argType, const SymbolTable& table);

/**
 * @brief Tests whether a type declares or inherits an operator accepting the argument type.
 * @param[in] typeName Type that owns the operator.
 * @param[in] opName Operator method name such as `opAdd`.
 * @param[in] argType Argument type passed to the operator.
 * @param[in] table Symbol table to search.
 * @return True when a compatible overload exists.
 */
bool TypeHasOperator(std::string_view typeName, std::string_view opName, std::string_view argType,
                     const SymbolTable& table);

/**
 * @brief Tests whether a type converts implicitly to a target through opImplConv or opConv.
 * @param[in] typeName Source type.
 * @param[in] targetType Destination type.
 * @param[in] table Symbol table to search.
 * @return True when a conversion operator to the target exists.
 */
bool TypeHasOpImplConvTo(std::string_view typeName, std::string_view targetType, const SymbolTable& table);

/**
 * @brief Tests whether a type name is declared or built in.
 * @param[in] type Type spelling.
 * @param[in] table Symbol table to search.
 * @param[in] stringTypeName Configured string type name.
 * @return True when the type is known.
 */
bool IsKnownType(std::string_view type, const SymbolTable& table, std::string_view stringTypeName);

/**
 * @brief Tests whether an operand is the null literal or resolved to null.
 * @param[in] operand Operand facts.
 * @param[in] type Resolved operand type spelling.
 * @return True when the operand is null.
 */
bool IsNullOperand(const BinaryOperandFact& operand, std::string_view type);

/**
 * @brief Resolves both operand types of a binary expression and strips their modifiers.
 * @param[in] fact Binary expression facts.
 * @param[in] scope Lexical scope enclosing the expression.
 * @param[in] ctx Diagnostic context.
 * @return Clean operand types, or nullopt when either is unresolved or poisoned.
 */
std::optional<OperandTypePair> ResolveCleanBinaryOperandTypes(const BinaryExpressionFact& fact, const Scope* scope,
                                                              const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
