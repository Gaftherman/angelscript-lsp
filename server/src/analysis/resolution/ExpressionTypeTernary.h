#pragma once

#include "analysis/index/SymbolTable.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves result type between ternary consequence and alternative branch types.
 * @param[in] t1 Consequence branch type name.
 * @param[in] t2 Alternative branch type name.
 * @param[in] symbolTable Symbol table for hierarchy and conversion checks.
 * @return Resolved common branch type name, or empty string.
 */
[[nodiscard]] std::string ResolveTernaryBranchTypes(std::string_view t1, std::string_view t2,
                                                    const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
