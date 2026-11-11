#pragma once

#include "analysis/index/SymbolTable.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves constructor call type given written type and optional template arguments.
 * @param[in] typeText Type name token spelling.
 * @param[in] templateText Template argument list spelling, if present.
 * @return Cleaned instantiated type name, or empty string.
 */
[[nodiscard]] std::string ResolveConstructCallType(std::string_view typeText,
                                                   std::string_view templateText = "");

/**
 * @brief Resolves cast expression type from the target type operand spelling.
 * @param[in] typeText Target cast type spelling.
 * @return Cleaned target type name.
 */
[[nodiscard]] std::string ResolveCastExpressionType(std::string_view typeText);

/**
 * @brief Resolves lambda target funcdef type name from target name.
 * @param[in] targetName Target funcdef or function signature name.
 * @param[in] symbolTable Symbol table to query.
 * @return Qualified funcdef type name, or empty string.
 */
[[nodiscard]] std::string ResolveLambdaTargetFuncdefType(std::string_view targetName,
                                                         const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
