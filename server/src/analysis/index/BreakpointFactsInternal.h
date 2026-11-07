#pragma once

#include "analysis/index/BreakpointFacts.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Classifies and emits breakpoint spans for an AST statement or declaration node.
 * @param[in] node Tree-Sitter AST node to inspect.
 * @param[in] sym Grammar symbol identifier of node.
 * @param[in,out] ctx Breakpoint span collection context.
 */
void ProcessBreakpointStatement(TSNode node, TSSymbol sym, BreakpointSpanContext& ctx);

} // namespace angel_lsp::analysis
