#pragma once

#include "analysis/index/SymbolTable.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if targetName refers to a function symbol (candidate for function handle take).
 * @param[in] targetName Name of the operand target.
 * @param[in] inScopeSymbols Symbols in the active lexical scope.
 * @param[in] symbolTable Symbol table to query.
 * @return True if targetName refers to a function.
 */
[[nodiscard]] bool IsFunctionHandleTargetName(std::string_view targetName,
                                             const std::vector<Symbol>& inScopeSymbols,
                                             const SymbolTable& symbolTable);

/**
 * @brief Resolves the resulting type of a unary minus expression.
 * @param[in] operandType Inferred type of the inner operand.
 * @return Signed equivalent for unsigned integers, or the original type.
 */
[[nodiscard]] std::string ResolveUnaryMinus(std::string_view operandType);

/**
 * @brief Resolves the resulting type of a unary handle take expression ('@expr').
 * @param[in] isFunctionTarget True if the operand targets a function symbol.
 * @param[in] operandType Inferred type of the inner operand.
 * @return Handle type ending with '@', poison type, or empty string.
 */
[[nodiscard]] std::string ResolveUnaryHandle(bool isFunctionTarget, std::string_view operandType);

} // namespace angel_lsp::analysis
