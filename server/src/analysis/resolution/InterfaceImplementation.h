#pragma once

#include "analysis/index/SymbolTable.h"

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
} // namespace angel_lsp::analysis
