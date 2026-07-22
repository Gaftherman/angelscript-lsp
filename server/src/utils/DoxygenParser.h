/**
 * @file DoxygenParser.h
 * @brief Utilities for parsing raw Doxygen comment blocks into structured data and Markdown.
 * @ingroup Utils
 */

#pragma once

#include <string>
#include <vector>
#include "i18n/LspStrings.h"

namespace angel_lsp::utils
{
    /**
     * @brief A single parameter documentation entry extracted from Doxygen comments.
     */
    struct DoxygenParam
    {
        std::string name;
        std::string description;
    };

    /**
     * @brief Structured neutral representation of parsed Doxygen comments.
     */
    struct ParsedDoxygenDoc
    {
        std::string brief;
        std::string details;
        std::string note;
        std::string warning;
        std::string deprecated;
        std::string returns;
        std::vector<DoxygenParam> parameters;
    };

    /**
     * @brief Parses a raw Doxygen comment string into a structured representation.
     *
     * @param[in] rawDoxygen The raw Doxygen string (e.g. "/** @brief ... *\/").
     * @return ParsedDoxygenDoc Structured object containing extracted fields.
     * @note Thread-safe stateless string parsing function.
     */
    ParsedDoxygenDoc ParseDoxygenComment(const std::string &rawDoxygen);

    /**
     * @brief Formats raw Doxygen documentation into localized Markdown text.
     *
     * @param[in] rawDoxygen The raw Doxygen documentation comment string.
     * @param[in] locale Target localization locale (e.g. Locale::EN_US or Locale::ES_ES).
     * @param[in] targetParam Optional parameter name to highlight or filter.
     * @return std::string Formatted Markdown documentation text.
     * @note Thread-safe stateless formatting function.
     */
    std::string FormatDoxygenToMarkdown(const std::string &rawDoxygen, i18n::Locale locale, const std::string &targetParam = "");
}
