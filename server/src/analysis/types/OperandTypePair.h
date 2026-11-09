#pragma once

#include "analysis/index/Symbol.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Resolved type spellings of the two operands of a binary expression.
 * @note Owned text: resolution returns temporaries that do not view the document.
 */
struct OperandTypePair
{
    std::string left;
    std::string right;
};

/**
 * @brief Copies two type spellings into an owned pair.
 * @param[in] left Left operand type spelling.
 * @param[in] right Right operand type spelling.
 * @return Pair that owns copies of both spellings.
 */
[[nodiscard]] inline OperandTypePair MakeOperandTypePair(std::string_view left, std::string_view right)
{
    return OperandTypePair{std::string(left), std::string(right)};
}

/**
 * @brief One operand that converts implicitly, kept until repeated conversions are compared.
 */
struct ConversionOccurrence
{
    SourceRange range;         ///< Span of the converted operand.
    std::string_view exprText; ///< Operand spelling; views the analysed document text.
    OperandTypePair types;     ///< Source type in `left`, destination type in `right`.
};

} // namespace angel_lsp::analysis
