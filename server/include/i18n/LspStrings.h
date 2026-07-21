#pragma once

#include <string>

namespace i18n
{

    /**
     * @brief Enum representing the supported locales for localization.
     */
    enum class Locale
    {
        EN, // English (default)
        ES, // Español
        UNKNOWN = EN
    };

    /**
     * @brief Structure containing all localized strings for the LSP.
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
        
        // --- Prefijos Semánticos para Hover ---
        std::string_view hoverParameter;
        std::string_view hoverProperty;
        std::string_view hoverField;
        std::string_view hoverLocalVariable;
        std::string_view hoverEnumMember;
    };

    /**
     * @brief Parses a locale string from the LSP client (e.g. "en", "es-419") into the internal enum.
     *
     * @param localeStr The raw locale string provided by the LSP.
     * @return Locale The resolved locale.
     */
    Locale ParseLocale(const std::string &localeStr);

    /**
     * @brief Gets the localized string table for the specified locale.
     *
     * @param locale The target locale (defaults to EN).
     * @return const LspStrings& Reference to the static strings structure for that locale.
     */
    const LspStrings &GetStrings(Locale locale = Locale::EN);

} // namespace i18n
