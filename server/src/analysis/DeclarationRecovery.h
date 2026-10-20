#pragma once

#include "analysis/Diagnostics.h"
#include <ankerl/unordered_dense.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

struct Symbol;

/**
 * @brief Represents an isolated declaration snippet extracted from source text.
 */
struct RecoveredDeclarationSnippet
{
    /** @brief The isolated snippet text (e.g., class Body { ... }). */
    std::string snippet;
    /** @brief Name of the declaration. */
    std::string name;
    /** @brief Kind of declaration (e.g., "class", "interface", "enum", "funcdef"). */
    std::string kind;
    /** @brief Absolute start byte offset in the parent document. */
    size_t startByte = 0;
    /** @brief 0-indexed start line in the parent document. */
    size_t startLine = 0;
    /** @brief 0-indexed column within the start line. */
    size_t startCol = 0;
};

/**
 * @brief Finds the terminating byte offset of a declaration block starting at declStartByte.
 *
 * For braces-delimited blocks (class, interface, enum), matches balanced curly braces
 * skipping comments, character literals, and strings (including multiline triple-quoted heredocs).
 * For semicolon-delimited declarations (funcdef), matches the terminating semicolon.
 *
 * @param[in] sourceCode Document source text.
 * @param[in] declStartByte Byte offset where the declaration begins.
 * @param[in] keyword Keyword identifying the declaration kind ("class", "interface", "enum", "funcdef").
 * @return Byte offset past the terminating token (closing brace or semicolon), or std::string_view::npos on failure.
 */
[[nodiscard]] size_t FindBalancedDeclarationEnd(std::string_view sourceCode, size_t declStartByte,
                                                std::string_view keyword);

/**
 * @brief Scans source code for top-level declaration headers that are missing from knownSymbols.
 *
 * Intended for invocation only when the primary AST has syntax errors (e.g. unclosed string
 * or heredoc literals), rescuing declarations swallowed into string tokens.
 *
 * @param[in] sourceCode Document source text.
 * @param[in] knownSymbols Set of symbol names already successfully collected.
 * @return Vector of extracted declaration snippets ready for isolated parsing.
 */
[[nodiscard]] std::vector<RecoveredDeclarationSnippet>
FindSwallowedDeclarations(std::string_view sourceCode, const ankerl::unordered_dense::set<std::string>& knownSymbols);

/**
 * @brief Offsets symbol line and column ranges by the snippet's origin in the parent document.
 *
 * @param[in,out] sym Symbol whose ranges will be shifted.
 * @param[in] startLine 0-indexed line offset of the snippet.
 * @param[in] startCol 0-indexed column offset of the snippet on its first line.
 */
void AdjustRecoveredSymbolOffsets(Symbol& sym, size_t startLine, size_t startCol);

} // namespace angel_lsp::analysis
