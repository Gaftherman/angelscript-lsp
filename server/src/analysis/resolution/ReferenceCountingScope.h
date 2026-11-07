#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/ReferenceCounting.h"

#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Finds the innermost class or interface enclosing a line, filling the per-file span cache on first use.
 * @param[in] symbolTable Global symbol table.
 * @param[in] uri Document file URI.
 * @param[in] line Document line.
 * @param[in,out] cache Per-file class span cache that owns the returned name.
 * @return View into cache of the enclosing class name, valid while cache lives; empty if none.
 */
[[nodiscard]] std::string_view GetEnclosingClassName(const SymbolTable& symbolTable, std::string_view uri,
                                                     uint32_t line, EnclosingClassCache& cache);

/**
 * @brief Checks if a container symbol is a class or interface rather than a namespace.
 * @param[in] container Name of the container.
 * @param[in] symbolTable Global symbol table.
 * @return True if container is a class or interface.
 */
[[nodiscard]] bool IsContainerClass(std::string_view container, const SymbolTable& symbolTable);

/**
 * @brief Checks if a symbol belongs to a class or interface container.
 * @param[in] sym Target symbol.
 * @param[in] symbolTable Global symbol table.
 * @return True if container is a class or interface.
 */
[[nodiscard]] bool IsSymbolInClassContainer(const Symbol& sym, const SymbolTable& symbolTable);

/**
 * @brief Checks whether a reference at given line in fileUri is inside a named namespace.
 * @param[in] fileUri Document URI.
 * @param[in] line Reference line.
 * @param[in] nsName Namespace name.
 * @param[in] symbolTable Global symbol table.
 * @param[in,out] classCache Per-file class span cache.
 * @return True if inside namespace block.
 */
[[nodiscard]] bool IsInsideNamespace(std::string_view fileUri, uint32_t line, std::string_view nsName,
                                     const SymbolTable& symbolTable, EnclosingClassCache& classCache);
} // namespace angel_lsp::analysis
