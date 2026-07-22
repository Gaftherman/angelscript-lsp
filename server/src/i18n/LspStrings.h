/**
 * @file LspStrings.h
 * @brief Localized string tables and locale parsing declarations.
 * @ingroup i18n
 */

#pragma once

#include <string>
#include <string_view>

namespace i18n
{

    /**
     * @brief Supported locales for diagnostic and hover UI translations.
     */
    enum class Locale
    {
        EN, // English (default)
        ES, // Español
        UNKNOWN = EN,
        EN_US = EN,
        ES_ES = ES
    };

    /**
     * @brief Localized Doxygen section header strings.
     */
    struct DoxygenHeaderStrings
    {
        std::string_view parameters;     // "Parameters:" / "Parámetros:"
        std::string_view typeParameters; // "Type Parameters:" / "Parámetros de Tipo:"
        std::string_view returns;        // "Returns:" / "Retorna:"
        std::string_view exceptions;     // "Exceptions:" / "Excepciones:"
        std::string_view warning;        // "Warning:" / "Advertencia:"
        std::string_view deprecated;     // "Deprecated:" / "Obsoleto:"
        std::string_view note;           // "Note:" / "Nota:"
        std::string_view seeAlso;        // "See also:" / "Ver también:"
    };

    /**
     * @brief Structure containing localized strings for symbol kinds, hover labels, and Doxygen headers.
     */
    struct LspStrings
    {
        // --- Kind names ---
        std::string_view kindVariable;
        std::string_view kindFunction;
        std::string_view kindClass;
        std::string_view kindNamespace;
        std::string_view kindParameter;
        std::string_view kindProperty;
        std::string_view kindMethod;
        std::string_view kindEnum;
        std::string_view kindEnumMember;
        std::string_view kindInterface;
        std::string_view kindFuncdef;
        std::string_view kindMixin;
        std::string_view kindTypedef;
        std::string_view kindConstructor;
        std::string_view kindDestructor;
        std::string_view kindUnknown;

        // --- Hover labels ---
        std::string_view hoverIn;
        std::string_view hoverBuiltinFunc;
        std::string_view hoverBuiltinType;
        std::string_view hoverEngineError;
        std::string_view hoverEngineWarn;
        std::string_view hoverUnresolved;
        std::string_view hoverAmbiguous;
        std::string_view hoverDefinedIn;
        std::string_view hoverAliasTo;

        // --- Hover Doxygen ---
        std::string_view hoverTemplateParams;
        std::string_view hoverParams;
        std::string_view hoverReturns;
        std::string_view hoverThrows;
        std::string_view hoverNote;
        std::string_view hoverWarning;
        std::string_view hoverDeprecated;
        std::string_view hoverOverloads;
        std::string_view hoverParameterOf;
        std::string_view hoverNeverUsed;
        
        // --- Semantic Hover Prefixes ---
        std::string_view hoverParameter;
        std::string_view hoverProperty;
        std::string_view hoverField;
        std::string_view hoverLocalVariable;
        std::string_view hoverEnumMember;
        std::string_view hoverIncludedFile;
    };

    /**
     * @brief Parses a raw locale string into the internal Locale enum.
     *
     * @param[in] localeStr Raw locale string (e.g., "en", "es-ES", "es-419").
     * @return Locale Resolved Locale enum.
     * @note Thread-safe stateless locale parser.
     */
    Locale ParseLocale(const std::string &localeStr);

    /**
     * @brief Gets the static localized string table for a specified locale.
     *
     * @param[in] locale Target locale enum (defaults to Locale::EN).
     * @return const LspStrings& Static reference to the localized string table.
     * @note Thread-safe for concurrent read access.
     */
    const LspStrings &GetStrings(Locale locale = Locale::EN);

    /**
     * @brief Gets localized Doxygen header strings for a specified locale.
     *
     * @param[in] locale Target locale enum.
     * @return DoxygenHeaderStrings Localized header strings struct.
     */
    DoxygenHeaderStrings GetDoxygenHeaders(Locale locale = Locale::EN);

} // namespace i18n

namespace angel_lsp::i18n
{
    using ::i18n::Locale;
    using ::i18n::LspStrings;
    using ::i18n::DoxygenHeaderStrings;
    using ::i18n::ParseLocale;
    using ::i18n::GetStrings;
    using ::i18n::GetDoxygenHeaders;
}
