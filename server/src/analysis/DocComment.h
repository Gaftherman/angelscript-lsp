#pragma once

#include <cstdint>
#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Extracts and formats the doc comment preceding a declaration.
     *
     * Understands both `///`/`//` runs and `/** ... *\/` blocks, and renders the Doxygen tags a
     * declaration usually carries (@brief, @param, @return, @note, @warning, @see) as markdown.
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
