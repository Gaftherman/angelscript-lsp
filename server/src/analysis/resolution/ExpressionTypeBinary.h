#pragma once

#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether a binary operator produces a boolean result.
 * @param[in] op Binary operator spelling.
 * @return True if operator is comparison or boolean logic.
 */
[[nodiscard]] bool IsBooleanBinaryOp(std::string_view op) noexcept;

/**
 * @brief Determines primitive numeric promotion result for binary expressions.
 * @param[in] op Binary operator string.
 * @param[in] cleanLeft Cleaned left operand type.
 * @param[in] cleanRight Cleaned right operand type.
 * @param[in] disableIntegerDivision True if integer division promotion is disabled.
 * @param[in] symbolTable Symbol table for enum checks.
 * @return Promoted type name, or empty string.
 */
[[nodiscard]] std::string ResolveBinaryPrimitivePromotion(std::string_view op, std::string_view cleanLeft,
                                                          std::string_view cleanRight,
                                                          bool disableIntegerDivision = false,
                                                          const SymbolTable* symbolTable = nullptr);

/**
 * @brief Resolves user-defined operator overloads for a binary expression.
 * @param[in] op Operator string.
 * @param[in] leftType Left operand type.
 * @param[in] rightType Right operand type.
 * @param[in] symbolTable Symbol table for member lookup.
 * @return Resolved return type, or empty string if no overload matches.
 */
[[nodiscard]] std::string ResolveBinaryOverload(std::string_view op, std::string_view leftType,
                                                std::string_view rightType, const SymbolTable& symbolTable);

/**
 * @brief Evaluates the resulting type of a binary operation between two operand types.
 * @param[in] op Binary operator spelling.
 * @param[in] leftType Left operand type.
 * @param[in] rightType Right operand type.
 * @param[in] ctx Semantic expression context.
 * @return Resulting expression type name.
 */
[[nodiscard]] std::string EvaluateBinaryResultType(std::string_view op, std::string_view leftType,
                                                   std::string_view rightType, const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
