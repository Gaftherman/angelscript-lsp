#pragma once

#include "analysis/index/SymbolTable.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Searches a collection of accessor symbols for an indexed getter return type.
 * @param[in] accessors Vector of accessor symbols.
 * @param[in] symbolTable Global symbol table pointer for overload resolution.
 * @param[in] indexArgType Deduced type of the index argument.
 * @return Return type of the indexed getter, or empty string.
 */
[[nodiscard]] std::string FindIndexedGetterType(const std::vector<Symbol>& accessors,
                                                const SymbolTable* symbolTable = nullptr,
                                                std::string_view indexArgType = "");

/**
 * @brief Resolves indexed property accessor type for member receiver types.
 * @param[in] recType Receiver object type name.
 * @param[in] memName Member property name.
 * @param[in] symbolTable Symbol table to query.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
[[nodiscard]] std::string ResolveMemberIndexedPropertyType(std::string_view recType,
                                                           std::string_view memName,
                                                           const SymbolTable& symbolTable,
                                                           std::string_view indexArgType = "");

/**
 * @brief Resolves indexed property accessor type from scoped name components.
 * @param[in] scopeName Scope qualifier portion of the identifier.
 * @param[in] name Unqualified member name.
 * @param[in] symbolTable Symbol table to query.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
[[nodiscard]] std::string ResolveScopedIndexedPropertyType(std::string_view scopeName,
                                                           std::string_view name,
                                                           const SymbolTable& symbolTable,
                                                           std::string_view indexArgType = "");

/**
 * @brief Resolves indexed property accessor type from bare identifier.
 * @param[in] name Identifier name.
 * @param[in] enclosingClass Enclosing class name if within a class body, or empty.
 * @param[in] symbolTable Symbol table to query.
 * @param[in] indexArgType Inferred type of the index argument.
 * @return Resolved return type of the indexed getter, or empty string.
 */
[[nodiscard]] std::string ResolveBareIndexedPropertyType(std::string_view name,
                                                         std::string_view enclosingClass,
                                                         const SymbolTable& symbolTable,
                                                         std::string_view indexArgType = "");

} // namespace angel_lsp::analysis
