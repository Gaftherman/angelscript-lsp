#pragma once

#include "analysis/index/LocalConstFacts.h"
#include "analysis/mediator/SemanticMediator.h"

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether a local declaration has later writes or uses that require mutable storage.
 * @param[in] declaration Indexed, explicitly initialized variable declaration.
 * @param[in] facts Storage uses from the same syntax revision.
 * @param[in] mediator Current document scope, syntax, source, and workspace symbols.
 * @return True for a function local with no writes or uses that require mutable storage.
 * @note Scans the indexed operator sites and storage uses once, resolving only uses of the declared name.
 */
[[nodiscard]] bool CanQualifyLocalConst(const IndexedLocalConstDeclaration& declaration, const LocalConstFacts& facts,
                                        const SemanticMediator& mediator);

} // namespace angel_lsp::analysis
