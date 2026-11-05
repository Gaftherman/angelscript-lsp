#pragma once

#include "analysis/index/ScopeTree.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a linked editing range request.
 */
struct LinkedEditingRangeRequest
{
    /**
     * @brief Document source text view to validate non-empty content.
     */
    std::string_view sourceCode;

    /**
     * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
     */
    const analysis::Scope* scopeRoot = nullptr;

    /**
     * @brief Cursor position within the document.
     */
    lsp::Position position;
};

/**
 * @brief Resolves linked editing ranges for an identifier under the cursor.
 *        Restricted to function-local variables and parameters to guarantee scope containment.
 * @param[in] request Input parameters containing scope tree, source view, and cursor position.
 * @return Linked editing ranges, or std::nullopt when the cursor is not on an eligible local symbol.
 */
std::optional<lsp::LinkedEditingRanges> GetLinkedEditingRanges(const LinkedEditingRangeRequest& request);
} // namespace angel_lsp::features
