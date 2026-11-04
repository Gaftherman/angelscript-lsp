#pragma once

#include "analysis/index/LocalScopeCollector.h"

namespace angel_lsp::analysis
{
/**
 * @brief Captures receiver and qualifier facts for one identifier without retaining syntax nodes.
 * @param[in] parent Parent of the identifier in the syntax tree.
 * @param[in] source Parsed document text.
 * @param[in,out] reference Occurrence with the identifier range and classification already set.
 * @param[in,out] context Scope extraction state for this document.
 */
void IndexReferenceReceiver(TSNode parent, std::string_view source, LocalReference& reference,
                            LocalScopeCollector::DocumentContext& context);
} // namespace angel_lsp::analysis
