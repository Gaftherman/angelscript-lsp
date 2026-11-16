#pragma once

#include "analysis/diagnostics/DiagnosticContext.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Inputs describing a declared type or address-of operand to validate for illegal handles.
 */
struct IllegalHandleCheckRequest
{
    std::string_view writtenType{};  ///< Full written type expression (e.g. "string@", "string@[]", "array<string@>").
    std::string_view baseTypeName{}; ///< Pre-extracted base type name or resolved operand value type.
    std::string_view enclosingNs{};  ///< Enclosing namespace of the declaration or expression.
    bool isAddressOfOperand = false; ///< True when checking the operand of unary '@' rather than a type spelling.
    bool hasPrimitiveHandle = false; ///< Pre-extracted primitive handle flag from First Pass.
    bool checkPrimitiveFlag = true;  ///< Whether to honor hasPrimitiveHandle directly.
};

/**
 * @brief Outcome of validating whether a type prohibits object handles ('@').
 */
struct IllegalHandleCheckResult
{
    std::string_view diagnosticCode{}; ///< Empty when legal; otherwise HandleOnPrimitive or HandleOnNoHandleType.
    std::string offendingTypeName{};   ///< Resolved offending type name for diagnostic message formatting.

    /**
     * @brief Reports whether an illegal handle usage was detected.
     * @return True if diagnosticCode is non-empty.
     */
    [[nodiscard]] bool HasError() const noexcept
    {
        return !diagnosticCode.empty();
    }
};

/**
 * @brief Validates a declared type or unary handle-of operand against primitive and configured no-handle type rules.
 * @param[in] req Handle check request describing the type spelling, base name, and context.
 * @param[in] ctx Diagnostic context providing the symbol table and engine rules configuration.
 * @return Diagnostic code and offending type name when handles are disallowed; empty result otherwise.
 */
[[nodiscard]] IllegalHandleCheckResult CheckIllegalHandleType(const IllegalHandleCheckRequest& req,
                                                              const DiagnosticContext& ctx);

} // namespace angel_lsp::analysis
