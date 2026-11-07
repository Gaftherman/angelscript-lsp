#pragma once

#include "analysis/index/CallContextFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"

#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves candidate callable symbols for an indexed call context.
 *
 * Dispatches to member, qualified, constructor, or direct candidate resolution
 * based on the indexed call context classification.
 *
 * @param[in] context Indexed call context facts.
 * @param[in] source Document text buffer.
 * @param[in] scope Lexical scope at the cursor, or nullptr.
 * @param[in] symbolTable Symbol table.
 * @return List of matching function or funcdef symbols.
 */
std::vector<Symbol> ResolveCallCandidates(const IndexedCallContext& context, std::string_view source,
                                          const Scope* scope, const SymbolTable& symbolTable);

} // namespace angel_lsp::analysis
