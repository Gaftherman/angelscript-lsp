#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ExpressionTypeResolver.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Resolves the type of a member-access receiver from its syntax, for completion after a dot.
 *        The expression type is tried first with the symbol table's string and array type names; a result
 *        of `void` or `unknown` falls back to receiver-type resolution in the same scope and document.
 * @param[in] receiverRange Byte range of the receiver expression, as returned by FindMemberReceiverRange.
 * @param[in] ctx Expression context; its node index supplies the tree, its scope, symbol table, source
 *            and URI drive the resolution.
 * @return Raw receiver type name, or an empty string when the context has no tree, no node covers the
 *         range, or neither resolution succeeds.
 */
[[nodiscard]] std::string ResolveCompletionReceiverType(IndexedByteRange receiverRange,
                                                        const ExpressionTypeContext& ctx);

} // namespace angel_lsp::analysis
