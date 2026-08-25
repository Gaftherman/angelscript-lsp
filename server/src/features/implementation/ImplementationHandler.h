#pragma once

#include "analysis/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <tree_sitter/api.h>

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    /**
     * @brief Context and input parameters for an implementation request.
     */
    struct ImplementationRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        lsp::Position position;
    };

    /**
     * @brief Resolves what implements or overrides the symbol under the cursor.
     *
     * Definition answers "where is this declared"; this answers the opposite question, "who
     * answers to it", which in AngelScript means the direction interfaces are actually read in.
     * Two shapes, and the cursor decides which:
     *
     * - on the name of an interface or a class, the types that derive from it, transitively;
     * - on a method declared in one, the declarations of that method in those types.
     *
     * A type that nothing derives from answers with nothing rather than with itself: an
     * implementation list containing only the declaration the cursor is already on is a worse
     * answer than an empty one, because the editor jumps and nothing has changed.
     *
     * @param request Document, tree, symbol table and cursor position.
     * @return Locations of the implementations, or nullopt when the cursor is not on a name this
     *         question applies to.
     */
    std::optional<std::vector<lsp::Location>> GetImplementations(const ImplementationRequest &request);
}
