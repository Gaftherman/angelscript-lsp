#pragma once

#include "analysis/formatting/FormattingTypes.h"
#include "analysis/index/NodeIndex.h"

#include <lsp/types.h>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and options for whole-document formatting.
 * @warning `uri` and `sourceCode` view request-owned text; the request must not outlive the document handle.
 */
struct FormattingRequest
{
    std::string_view uri{};                              ///< Document URI, used to find the nearest `.clang-format`.
    std::string_view sourceCode{};                       ///< Document text to format.
    analysis::formatting::FormatLayoutOptions options{}; ///< Editor whitespace settings.
    analysis::formatting::BraceStyle braceStyle = analysis::formatting::BraceStyle::Allman; ///< Brace placement.
    bool spacesInsideParentheses = false;     ///< Pad the inside of parentheses.
    bool keepEmptyBlocksOnSingleLine = false; ///< Keep `{}` on one line.
    analysis::formatting::PointerAlignment pointerAlignment =
        analysis::formatting::PointerAlignment::Left; ///< Where `@` attaches in declarations.
};

/**
 * @brief Context and options for range formatting.
 * @warning `uri` and `sourceCode` view request-owned text; the request must not outlive the document handle.
 */
struct RangeFormattingRequest
{
    std::string_view uri{};                              ///< Document URI, used to find the nearest `.clang-format`.
    std::string_view sourceCode{};                       ///< Document text to format.
    lsp::Range range{};                                  ///< Requested range; only its start and end lines matter.
    analysis::formatting::FormatLayoutOptions options{}; ///< Editor whitespace settings.
    analysis::formatting::BraceStyle braceStyle = analysis::formatting::BraceStyle::Allman; ///< Brace placement.
    bool spacesInsideParentheses = false;     ///< Pad the inside of parentheses.
    bool keepEmptyBlocksOnSingleLine = false; ///< Keep `{}` on one line.
    analysis::formatting::PointerAlignment pointerAlignment =
        analysis::formatting::PointerAlignment::Left; ///< Where `@` attaches in declarations.
};

/**
 * @brief Context and options for on-type formatting after `;` or `}`.
 * @warning The views and `nodeIndex` point into the document revision the request was built from.
 */
struct OnTypeFormattingRequest
{
    std::string_view uri{};                              ///< Document URI, used to find the nearest `.clang-format`.
    std::string_view sourceCode{};                       ///< Document text to format.
    const analysis::NodeIndex* nodeIndex = nullptr;      ///< Index of the parsed revision; null skips the syntax check.
    lsp::Position position{};                            ///< Position of the typed character.
    std::string_view ch{};                               ///< Typed character; only `;` and `}` trigger formatting.
    analysis::formatting::FormatLayoutOptions options{}; ///< Editor whitespace settings.
    analysis::formatting::BraceStyle braceStyle = analysis::formatting::BraceStyle::Allman; ///< Brace placement.
    bool spacesInsideParentheses = false;     ///< Pad the inside of parentheses.
    bool keepEmptyBlocksOnSingleLine = false; ///< Keep `{}` on one line.
    analysis::formatting::PointerAlignment pointerAlignment =
        analysis::formatting::PointerAlignment::Left; ///< Where `@` attaches in declarations.
};
} // namespace angel_lsp::features
