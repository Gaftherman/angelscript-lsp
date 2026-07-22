#pragma once

#include <string>
#include <vector>
#include "i18n/LspStrings.h"

namespace angel_lsp::utils
{
    /**
     * @brief A single parameter documentation entry extracted from Doxygen.
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
     * @brief Parses a raw Doxygen comment string into a structured neutral Doxygen representation.
     * @param rawDoxygen The raw Doxygen string (e.g., "/** @brief ... *\/").
     * @return Structured ParsedDoxygenDoc object containing extracted sections.
     */
    ParsedDoxygenDoc ParseDoxygenComment(const std::string &rawDoxygen);

    /**
     * @brief Formats raw Doxygen documentation into a Markdown string for a specified locale.
     * @param rawDoxygen The raw Doxygen documentation comment string.
     * @param locale Target localization locale.
     * @param targetParam Optional parameter filter.
     * @return Formatted Markdown documentation text.
     */
    std::string FormatDoxygenToMarkdown(const std::string &rawDoxygen, i18n::Locale locale, const std::string &targetParam = "");
}
