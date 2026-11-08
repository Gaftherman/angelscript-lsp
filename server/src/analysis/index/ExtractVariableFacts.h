#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <optional>

namespace angel_lsp::analysis
{

/**
 * @brief Categorization of syntax hints used to suggest candidate variable names.
 */
enum class ExtractNameHintKind : uint8_t
{
    /// No syntactic naming hint available.
    None,
    /// Callee name of a call (the member name when the callee is a member expression).
    CallCallee,
    /// Member name of a member expression.
    Member,
    /// Type of a construct call.
    ConstructType,
};

/**
 * @brief Indexed facts and source coordinates of an extract-variable candidate site.
 */
struct IndexedExtractVariableSite
{
    /// Byte range of the extractable target expression.
    IndexedByteRange expression;
    /// Source coordinate range of the extractable target expression.
    SourceRange expressionRange;
    /// Start line of the enclosing block statement.
    uint32_t statementLine = 0;
    /// Start column of the enclosing block statement.
    uint32_t statementColumn = 0;
    /// True when statement parent is a statement_block whose start row equals the statement row.
    bool statementSharesBlockLine = false;
    /// Classification of the naming hint extracted from the target expression.
    ExtractNameHintKind hintKind = ExtractNameHintKind::None;
    /// Byte range of the identifier or type name used for naming hints.
    IndexedByteRange hintName;
};

/**
 * @brief Locates an extractable expression and enclosing statement facts at a selection range.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] selection Source selection range to extract into a variable.
 * @return Extracted variable site facts if a valid expression and statement exist; std::nullopt otherwise.
 * @note Cost: O(depth) ancestor climbs and bounded field lookups in the syntax tree.
 */
[[nodiscard]] std::optional<IndexedExtractVariableSite> FindExtractVariableSite(const NodeIndex& index,
                                                                                const SourceRange& selection);

} // namespace angel_lsp::analysis
