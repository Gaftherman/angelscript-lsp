#pragma once

#include <string>

namespace i18n
{

/**
 * @brief Enum representing the supported locales for localization.
 */
enum class Locale
{
    EN,   // English (default)
    ES,   // Español
    UNKNOWN = EN
};

/**
 * @brief Structure containing all localized strings for the LSP.
 */
struct LspStrings
{
    // --- Kind names ---
    const char *kindVariable;
    const char *kindFunction;
    const char *kindClass;
    const char *kindNamespace;
    const char *kindParameter;
    const char *kindProperty;
    const char *kindMethod;
    const char *kindEnum;
    const char *kindEnumMember;
    const char *kindInterface;
    const char *kindFuncdef;
    const char *kindMixin;
    const char *kindTypedef;
    const char *kindConstructor;
    const char *kindDestructor;
    const char *kindUnknown;

    // --- Hover labels ---
    const char *hoverIn;
    const char *hoverBuiltinFunc;
    const char *hoverBuiltinType;
    const char *hoverEngineError;
    const char *hoverEngineWarn;
    const char *hoverUnresolved;
    const char *hoverAmbiguous;
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
const LspStrings& GetStrings(Locale locale = Locale::EN);

} // namespace i18n
