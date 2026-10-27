#pragma once

#include "parser/AngelScriptParser.h"
#include "utils/PreprocessorRegions.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Represents a valid 0-based source range where a breakpoint can be placed.
 */
struct BreakpointLocationItem
{
    uint32_t line = 0;
    uint32_t column = 0;
    uint32_t endLine = 0;
    uint32_t endColumn = 0;
};

/**
 * @brief Request bundle for querying valid breakpoint locations within a document range.
 */
struct BreakpointLocationsRequest
{
    TSNode rootNode{};
    uint32_t startLine = 0;
    uint32_t endLine = 0;
    const std::vector<angel_lsp::utils::ExcludedLineRange>* excludedRanges = nullptr;
};

/**
 * @brief Computes valid executable breakpoint locations from the Tree-Sitter AST.
 */
class BreakpointLocationsHandler
{
  public:
    /**
     * @brief Collects all valid breakpoint locations intersecting [startLine, endLine].
     * @param[in] request Immutable request containing the AST root, source, and line range.
     * @return Sorted, deduplicated vector of valid breakpoint locations.
     */
    static std::vector<BreakpointLocationItem> Collect(const BreakpointLocationsRequest& request);
};

} // namespace angel_lsp::features
