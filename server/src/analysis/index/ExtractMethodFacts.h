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
 * @brief Extracted facts and syntactic boundaries for an extract-method selection.
 */
struct IndexedExtractMethodSelection
{
    /// Start row of the enclosing function declaration.
    uint32_t functionStartLine = 0;
    /// End row of the enclosing function declaration.
    uint32_t functionEndLine = 0;
    /// Whether the function sits inside a class declaration.
    bool inClass = false;
    /// Class body closing brace line, valid when inClass is true.
    uint32_t classInsertLine = 0;
    /// Class body closing brace column, valid when inClass is true.
    uint32_t classInsertColumn = 0;
    /// Start byte of first selected statement to end byte of last.
    IndexedByteRange statements;
    /// Start point of first selected statement to end point of last.
    SourceRange statementsRange;
    /// True when the last selected statement is a return statement.
    bool endsWithReturn = false;
    /// return_type field of the function declaration, empty when absent.
    IndexedByteRange functionReturnType;
    /// Identifiers mutated inside the selected statements in traversal order.
    std::vector<IndexedByteRange> mutatedNames;
};

/**
 * @brief Analyzes a statement selection range within a function to extract method metadata.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] selection Source selection range to extract into a method.
 * @return Extracted method selection facts if valid statements are selected; std::nullopt otherwise.
 * @note Cost: O(depth) ancestor climbs and statement cursor scans in the syntax tree.
 */
[[nodiscard]] std::optional<IndexedExtractMethodSelection>
FindExtractMethodSelection(const NodeIndex& index, std::string_view sourceCode, const SourceRange& selection);

} // namespace angel_lsp::analysis
