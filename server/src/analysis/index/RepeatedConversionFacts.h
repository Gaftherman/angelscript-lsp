#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Outermost statement position and matching operand ranges in an if-ladder.
 */
struct IfLadderConversionSites
{
    /// Start row of the outermost if statement around the diagnostic.
    uint32_t rootIfLine = 0;
    /// Operand ranges in the ladder conditions whose text equals the converted expression.
    std::vector<SourceRange> occurrences;
};

/**
 * @brief Identifies conversion occurrences across conditions of an if-else ladder.
 * @param[in] index Syntax index containing the AST.
 * @param[in] sourceCode Source document content.
 * @param[in] diagnosticRange Range of the repeated conversion diagnostic.
 * @param[in] exprText Converted expression text to match.
 * @return Conversion sites if found; otherwise std::nullopt.
 * @note Cost: O(K * M) query matching across ladder condition nodes.
 */
[[nodiscard]] std::optional<IfLadderConversionSites> FindIfLadderConversionSites(const NodeIndex& index,
                                                                                 std::string_view sourceCode,
                                                                                 const SourceRange& diagnosticRange,
                                                                                 std::string_view exprText);

} // namespace angel_lsp::analysis
