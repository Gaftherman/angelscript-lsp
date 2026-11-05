#pragma once

#include "analysis/index/CallGraph.h"
#include "analysis/index/SymbolTable.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context for the request that opens a call hierarchy.
 */
struct CallHierarchyPrepareRequest
{
    std::string_view uri{};
    std::string_view sourceCode{};
    const analysis::SymbolTable& symbolTable;
    lsp::Position position{};
};

/**
 * @brief Context for a follow-up incoming or outgoing calls request.
 *        The client hands the item back, so neither the document nor its tree has to still be open -
 */
struct CallHierarchyItemRequest
{
    const analysis::SymbolTable& symbolTable;
    const analysis::CallGraphIndex& callGraph;
    const lsp::CallHierarchyItem& item;
};

/**
 * @brief Opens a call hierarchy on the function the cursor is looking at.
 *        The identifier under the cursor when it names a function - whether written at its declaration
 * @return The item, or nullopt when the cursor is looking at no function at all.
 */
[[nodiscard]] std::optional<std::vector<lsp::CallHierarchyItem>>
PrepareCallHierarchy(const CallHierarchyPrepareRequest& request);

/**
 * @brief The functions that call this one, each with the ranges of the calls inside it.
 * @note Matched by name. A call site says `Think()`; which declaration that reaches depends on
 *        the receiver's type, and an overload set or two same-named methods on unrelated classes
 */
[[nodiscard]] std::optional<std::vector<lsp::CallHierarchyIncomingCall>>
GetIncomingCalls(const CallHierarchyItemRequest& request);

/**
 * @brief The functions this one calls, each with the ranges of the calls to it.
 */
[[nodiscard]] std::optional<std::vector<lsp::CallHierarchyOutgoingCall>>
GetOutgoingCalls(const CallHierarchyItemRequest& request);
} // namespace angel_lsp::features
