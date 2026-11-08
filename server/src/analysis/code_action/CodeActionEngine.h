#pragma once

#include "analysis/code_action/CodeActionTypes.h"

#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Runs every code action provider for the query and collects their entries.
 * @param[in] query Protocol-free view of the document, cursor range and diagnostics.
 * @return Entries in provider order; empty when no action applies.
 */
[[nodiscard]] std::vector<CodeActionEntry> CollectCodeActionEntries(const CodeActionQuery& query);

} // namespace angel_lsp::analysis::code_action
