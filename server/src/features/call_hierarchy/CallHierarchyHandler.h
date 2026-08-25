#pragma once

#include "analysis/CallGraph.h"
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
     * @brief Context for the request that opens a call hierarchy.
     */
    struct CallHierarchyPrepareRequest
    {
        const std::string &uri;
        const std::string &sourceCode;
        TSTree *tree = nullptr;
        const analysis::SymbolTable &symbolTable;
        lsp::Position position;
    };

    /**
     * @brief Context for a follow-up incoming or outgoing calls request.
     *
     * The client hands the item back, so neither the document nor its tree has to still be open -
     * which matters, because a call hierarchy is walked across files the user never opened.
     */
    struct CallHierarchyItemRequest
    {
        const analysis::SymbolTable &symbolTable;
        const analysis::CallGraphIndex &callGraph;
        const lsp::CallHierarchyItem &item;
    };

    /**
     * @brief Opens a call hierarchy on the function the cursor is looking at.
     *
     * The identifier under the cursor when it names a function - whether written at its declaration
     * or at a call to it - and failing that, the function whose body the cursor sits in. The second
     * is what makes "show me who calls this" work from anywhere inside a body rather than only from
     * its signature.
     *
     * @return The item, or nullopt when the cursor is looking at no function at all.
     */
    std::optional<std::vector<lsp::CallHierarchyItem>> PrepareCallHierarchy(const CallHierarchyPrepareRequest &request);

    /**
     * @brief The functions that call this one, each with the ranges of the calls inside it.
     * @note Matched by name. A call site says `Think()`; which declaration that reaches depends on
     *       the receiver's type, and an overload set or two same-named methods on unrelated classes
     *       will therefore appear together. Over-reporting here costs the user a glance; resolving
     *       it wrongly would cost them a wrong answer with no way to see it was wrong.
     */
    std::optional<std::vector<lsp::CallHierarchyIncomingCall>> GetIncomingCalls(const CallHierarchyItemRequest &request);

    /**
     * @brief The functions this one calls, each with the ranges of the calls to it.
     */
    std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> GetOutgoingCalls(const CallHierarchyItemRequest &request);
}
