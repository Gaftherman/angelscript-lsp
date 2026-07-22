/**
 * @file HoverInfo.h
 * @brief Structured data representation and Markdown renderer for LSP hover panels.
 * @ingroup Features
 */

#pragma once
#include <string>
#include <vector>
#include <optional>
#include "analysis/Symbol.h"
#include "i18n/LspStrings.h"
#include "utils/DoxygenParser.h"

namespace angel_lsp::features::hover
{

    /**
     * @brief Structured data for a single parameter (function param or template param).
     */
    struct HoverParam
    {
        std::string typeName;       // "const float& in", "class T"
        std::string name;           // "value", "T"
        std::string defaultValue;   // "0.0f" or ""
        std::string docDescription; // from @param tag in doxygen
    };

    /**
     * @brief Structured representation of hover panel data, inspired by Clangd.
     * @note Thread-safe data structure for hover formatting.
     */
    struct HoverInfo
    {
        // --- Identity ---
        std::string name;
        analysis::SymbolKind kind = analysis::SymbolKind::Unknown;

        // --- Scope context ---
        std::string localScope;

        // --- Signature ---
        std::string rawSignature;

        // --- Structured function info ---
        std::optional<std::string> returnType;
        std::optional<std::vector<HoverParam>> parameters;
        std::optional<std::vector<HoverParam>> templateParameters;

        // --- Documentation ---
        std::string briefText;
        std::string detailsText;
        std::vector<std::string> notes;
        std::vector<std::string> warnings;
        std::string deprecated;
        std::string returnDoc;

        // --- Extras ---
        int overloadCount = 0;
        std::string templateSubstitution;
        std::string enumValue;
        std::string accessors;

        // --- Builtin info ---
        bool isBuiltin = false;
        std::string builtinLabel;

        // --- Diagnostics ---
        std::string diagnosticMessage;
        bool isDiagnosticError = false;

        struct HoverSection
        {
            bool isCodeBlock = false;
            std::string language;
            std::string content;
        };

        /**
         * @brief Populates documentation fields from a ParsedDoxygenDoc object.
         *
         * @param[in] doc The parsed Doxygen documentation.
         * @param[in] targetParam Optional target parameter filter string.
         */
        void PopulateFromDoxygen(const utils::ParsedDoxygenDoc &doc, const std::string &targetParam = "");

        /**
         * @brief Renders the structured hover panel data to a Markdown string.
         *
         * @param[in] locale Target localization locale.
         * @return std::string Formatted Markdown text.
         */
        std::string ToMarkdown(i18n::Locale locale) const;

        /**
         * @brief Converts hover panel data into a list of structured HoverSections.
         *
         * @param[in] locale Target localization locale.
         * @return std::vector<HoverSection> Vector of rendered hover sections.
         */
        std::vector<HoverSection> ToHoverSections(i18n::Locale locale) const;
    };

} // namespace angel_lsp::features::hover

namespace angel_lsp::features
{
    using hover::HoverInfo;
    using hover::HoverParam;
}
