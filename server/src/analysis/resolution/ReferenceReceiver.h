#pragma once

#include "analysis/index/ScopeTree.h"

namespace angel_lsp::analysis
{
/**
 * @brief Resolves a captured member receiver against its lexical scope and the current declarations.
 * @param[in] reference Member occurrence whose receiver was captured during scope indexing.
 * @param[in] scope Lexical scope of the use.
 * @param[in] symbols Declarations visible to the resolver.
 * @return The qualified receiver type, or an empty string if it cannot be established.
 */
std::string ResolveIndexedReferenceReceiver(const LocalReference& reference, const Scope& scope,
                                            const SymbolTable& symbols);
} // namespace angel_lsp::analysis
