#pragma once

#include "analysis/OverloadResolver.h"
#include "analysis/SymbolTable.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Context passed during argument-to-parameter type matching.
 */
struct MatchContext
{
    std::string cleanArg;
    std::string cleanParam;
    bool argIsHandle = false;
    bool argIsConst = false;
    bool paramIsHandle = false;
    bool paramIsConst = false;
    bool isMutableRef = false;
    const SymbolTable& table;
    std::string_view stringTypeName = "string";
    std::string_view arrayTypeName = "array";
};

/**
 * @brief Evaluates whether an argument count is viable for a function signature.
 * @param[in] sig Function signature.
 * @param[in] argCount Number of arguments supplied at call site.
 * @return True if arity is viable considering default and variadic parameters.
 */
[[nodiscard]] bool MatchesCallArity(const FunctionSignature& sig, uint32_t argCount) noexcept;

/**
 * @brief Finds the best fallback function candidate based on arity and argument conversion rank.
 * @param[in] candidates Candidate symbols.
 * @param[in] argTypes Deduced call site argument types.
 * @param[in] symbolTable Symbol table for type scoring.
 * @return Pointer to best matching symbol, or nullptr if none score positively.
 */
[[nodiscard]] const Symbol* FindBestFallbackOverload(
    const std::vector<Symbol>& candidates,
    const std::vector<std::string>& argTypes,
    const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
