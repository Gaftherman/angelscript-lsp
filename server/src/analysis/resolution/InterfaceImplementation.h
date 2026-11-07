#pragma once

#include "analysis/index/SymbolTable.h"
#include "i18n/i18n.h"

namespace angel_lsp::analysis
{
/**
 * @brief An interface obligation that a class still has to implement.
 */
struct MissingInterfaceMethod
{
    Symbol method;
    std::string interfaceName;
    bool canOverride = true;
};

/**
 * @brief Finds unsatisfied interface signatures using indexed members and inherited implementations.
 * @param[in] cls Class declaration to inspect.
 * @param[in] table Declarations visible in the class's module.
 * @return Missing methods in deterministic interface and signature order.
 */
std::vector<MissingInterfaceMethod> FindMissingInterfaceMethods(const Symbol& cls, const SymbolTable& table);

/**
 * @brief Checks whether a method symbol belongs to an interface container.
 * @param[in] sym Method symbol to inspect.
 * @param[in] table Symbol table for container lookup.
 * @return True if method belongs to an interface.
 */
[[nodiscard]] bool IsInterfaceMethod(const Symbol& sym, const SymbolTable& table);

/**
 * @brief Counts class implementations of an interface method.
 * @param[in] sym Interface method symbol.
 * @param[in] table Global symbol table.
 * @return Count of implementing class methods.
 */
[[nodiscard]] size_t CountInterfaceMethodImplementations(const Symbol& sym, const SymbolTable& table);

/**
 * @brief Counts classes that implement a given interface.
 * @param[in] sym Interface symbol.
 * @param[in] table Global symbol table.
 * @return Count of implementing classes.
 */
[[nodiscard]] size_t CountInterfaceImplementations(const Symbol& sym, const SymbolTable& table);

/**
 * @brief Formats localized implementation count for CodeLens.
 * @param[in] implCount Number of implementations.
 * @param[in] i18n Optional pointer to i18n translation context.
 * @return Formatted implementation count string.
 */
[[nodiscard]] std::string FormatImplementationCount(size_t implCount, const i18n::I18n* i18n);
} // namespace angel_lsp::analysis
