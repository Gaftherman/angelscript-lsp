#pragma once

#include "analysis/ScopeTree.h"

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
        const std::string &sourceCode;

        /** @brief Root of the document's lexical scope tree, or nullptr when none was collected. */
        const analysis::Scope *scopeRoot = nullptr;

        lsp::Position position;
    };

    /**
     * @brief Ranges the editor may retype together, live, without asking the server again.
     *
     * Deliberately narrow, and the narrowness is the point. The client edits every range it is
     * given as the user types, with no further round trip - so offering a range set that is not the
     * whole story silently desynchronises a project. Only two kinds of name can be answered safely:
     *
     * - a local variable, and
     * - a parameter,
     *
     * because a lexical scope cannot escape its document, so this file holds every occurrence there
     * is. Anything declared at file scope - a global, a class, a function, a field - can be
     * referenced from another file in the same module, and retyping only this file's occurrences
     * would leave the rest behind. Those go through prepareRename and rename, which look across
     * documents and produce one edit for all of them.
     *
     * @return The occurrences, or nullopt when the cursor is not on a name this can answer for.
     */
    std::optional<lsp::LinkedEditingRanges> GetLinkedEditingRanges(const LinkedEditingRangeRequest &request);
}
