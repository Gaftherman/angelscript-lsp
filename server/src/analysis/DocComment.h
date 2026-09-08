#pragma once

#include <cstdint>
#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Extracts and formats the doc comment preceding a declaration.
     *
     * Understands both `///`/`//` runs and `/\*\* ... *\/` blocks. Finding the comment is this
     * function's job; reading it is `RenderDoxygenMarkdown`'s, which parses it with the
     * tree-sitter-doxygen grammar and emits the Markdown clangd emits - no headings, bullets for
     * the parameters, `**Returns:**` for the return, block quotes for the notes and warnings.
     * What is passed on is the RAW comment, delimiters and leading stars included: the grammar
     * needs them to recognise a comment at all.
     *
     * Lives in the analysis layer rather than with hover because more than one feature shows
     * documentation - hover eagerly, completion when an item is resolved - and a feature may not
     * include another feature.
     *
     * @param sourceCode Text of the document the declaration is in.
     * @param declStartLine Zero-based line the declaration begins on.
     * @return Formatted markdown, or an empty string when the declaration carries no comment.
     */
    std::string ExtractDocComment(const std::string &sourceCode, uint32_t declStartLine);
}
