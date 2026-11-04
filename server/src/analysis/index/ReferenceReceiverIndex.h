#pragma once

#include "analysis/index/LocalScopeCollector.h"

namespace angel_lsp::analysis
{
/**
 * @brief Captures a named receiver chain with shared prefixes in document-local storage.
 * @param[in] node Receiver expression in the syntax tree.
 * @param[in] source Parsed document text.
 * @param[in,out] context Scope extraction state for this document.
 * @return Index of the captured receiver, or UINT32_MAX for an unsupported expression.
 */
uint32_t IndexReceiverExpression(TSNode node, std::string_view source, LocalScopeCollector::DocumentContext& context);
} // namespace angel_lsp::analysis
