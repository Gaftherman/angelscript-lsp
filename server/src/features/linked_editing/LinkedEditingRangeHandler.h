#pragma once

#include "analysis/index/ScopeTree.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a linked editing range request.
 */
struct LinkedEditingRangeRequest
{
    std::string sourceCode;

    /**
     * @brief Root of the document's lexical scope tree, or nullptr when none was collected.
     */
    const analysis::Scope* scopeRoot = nullptr;

    lsp::Position position;
};

/**
 * @brief Ranges the editor may retype together, live, without asking the server again.
 *        Deliberately narrow, and the narrowness is the point. The client edits every range it is
 * @return The occurrences, or nullopt when the cursor is not on a name this can answer for.
 */
std::optional<lsp::LinkedEditingRanges> GetLinkedEditingRanges(const LinkedEditingRangeRequest& request);
} // namespace angel_lsp::features
