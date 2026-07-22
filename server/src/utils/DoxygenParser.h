/**
 * @file DoxygenParser.h
 * @brief Tree-Sitter based C++20 Doxygen comment parser and Markdown formatter.
 * @ingroup Utils
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include "i18n/LspStrings.h"

namespace angel_lsp::utils
{
    /**
     * @brief Parameter documentation entry for function or template parameters.
     */
    struct DoxygenParam
    {
        std::string name;
        std::string direction; // "in", "out", "in,out"
        std::string description;
    };

    /**
     * @brief Complete structured representation of a parsed Doxygen comment block.
     */
    struct DoxygenDoc
    {
        std::string briefText;
        std::string detailsText;
        std::string returnDoc;
        std::string deprecatedDoc;

        std::vector<DoxygenParam> params;
        std::vector<DoxygenParam> tparams;
        std::vector<std::pair<std::string, std::string>> throwsDocs;

        std::vector<std::string> warnings;
        std::vector<std::string> notes;
        std::vector<std::string> seeAlso;

        // Generic fallback for any unclassified @tag (e.g. @author, @todo, @bug)
        std::vector<std::pair<std::string, std::string>> genericTags;

        // Backward compatibility fields for legacy callers
        std::string brief;
        std::string details;
        std::string returns;
        std::string deprecated;
        std::string note;
        std::string warning;
        std::vector<DoxygenParam> parameters;
    };

    /**
     * @brief Parses a raw Doxygen comment string into a structured DoxygenDoc object.
     *
     * @param[in] rawComment Raw Doxygen comment text.
     * @return DoxygenDoc Structured Doxygen representation.
     * @note Thread-safe stateless parsing function.
     */
    DoxygenDoc ParseDoxygen(std::string_view rawComment);

    /**
     * @brief Formats a DoxygenDoc structure into localized Markdown.
     *
     * @param[in] doc The parsed DoxygenDoc object.
     * @param[in] locale Target localization locale (defaults to Locale::EN).
     * @return std::string Formatted Markdown text.
     * @note Thread-safe stateless formatting function.
     */
    std::string FormatDoxygenToMarkdown(const DoxygenDoc &doc, i18n::Locale locale = i18n::Locale::EN);

    // --- Backward Compatibility Wrappers ---
    using ParsedDoxygenDoc = DoxygenDoc;

    /**
     * @brief Backward-compatible wrapper for ParseDoxygen.
     */
    DoxygenDoc ParseDoxygenComment(const std::string &rawDoxygen);

    /**
     * @brief Backward-compatible wrapper for formatting raw Doxygen comments to Markdown.
     */
    std::string FormatDoxygenToMarkdown(const std::string &rawDoxygen, i18n::Locale locale = i18n::Locale::EN, const std::string &targetParam = "");

} // namespace angel_lsp::utils
