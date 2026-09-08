#pragma once

#include <string>

namespace angel_lsp::analysis
{
    /**
     * @brief Parses a raw Doxygen comment and renders clangd-canonical Markdown for hover and completion.
     *
     * Converts Doxygen markup to structured Markdown following clangd's compact presentation:
     * brief paragraph, body paragraphs and code blocks in source order, template parameter bullets,
     * parameter bullets with optional directions, return section, and admonitions.
     *
     * @param rawComment The raw comment string, including delimiters (`/\*\* ... *\/` or a `///` run).
     * @return Formatted Markdown string, or empty if the comment carries no documentation content.
     */
    std::string RenderDoxygenMarkdown(const std::string &rawComment);
}
