#pragma once

#include "analysis/completion/CompletionTypes.h"
#include "analysis/index/SymbolTable.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::completion
{

/**
 * @brief Returns a document's text by URI, or nullptr when the host holds none.
 *        The pointer stays valid for as long as the host keeps the text alive; the reader decides that.
 */
using DocumentTextReader = std::function<const std::string*(const std::string&)>;

/**
 * @brief Computes the completion entries for the cursor in a query, sorted by their sort key.
 *        The first matching context decides: include path, inheritance, `::`, template arguments, `.` access,
 *        else every symbol, snippet and keyword in scope, re-ranked by the expected type when enabled.
 * @param[in] request Document, cursor and lookups for this completion.
 * @return Entries ready for protocol mapping; empty inside comments, strings, after a `case` label colon,
 *         or after a run of colons other than `::`.
 */
[[nodiscard]] std::vector<CompletionEntry> CollectCompletionEntries(const CompletionQuery& request);

/**
 * @brief Finds the documentation comment of a symbol picked from the completion list.
 *        Each declaration of the qualified name is tried in turn, reading the text of its file and
 *        extracting the comment above its first line.
 * @param[in] qualifiedName Qualified symbol name stored as the entry's resolve key.
 * @param[in] symbolTable Workspace symbols.
 * @param[in] readDocument Returns a document's text by URI, or nullptr when the host holds none.
 * @return Markdown documentation of the first declaration that has a comment, or std::nullopt when the name
 *         is empty, no reader is given, or no declaration has documentation.
 */
[[nodiscard]] std::optional<std::string> ResolveCompletionDocumentation(std::string_view qualifiedName,
                                                                        const SymbolTable& symbolTable,
                                                                        const DocumentTextReader& readDocument);

} // namespace angel_lsp::analysis::completion
