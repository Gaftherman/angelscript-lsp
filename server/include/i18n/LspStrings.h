#pragma once

#include <string>

namespace i18n {

enum class Locale {
    EN,   // English (default)
    ES,   // Español
    UNKNOWN = EN
};

struct LspStrings {
    // --- Kind names ---
    const char* kindVariable;
    const char* kindFunction;
    const char* kindClass;
    const char* kindNamespace;
    const char* kindParameter;
    const char* kindProperty;
    const char* kindMethod;
    const char* kindEnum;
    const char* kindEnumMember;
    const char* kindInterface;
    const char* kindFuncdef;
    const char* kindMixin;
    const char* kindTypedef;
    const char* kindConstructor;
    const char* kindDestructor;
    const char* kindUnknown;

    // --- Hover labels ---
    const char* hoverIn;
    const char* hoverBuiltinFunc;
    const char* hoverBuiltinType;
    const char* hoverEngineError;
    const char* hoverEngineWarn;
    const char* hoverUnresolved;
    const char* hoverAmbiguous;
};

// Parses the locale string from LSP ("en", "es-419", etc.)
Locale ParseLocale(const std::string& localeStr);

// Gets the string table for the given locale
const LspStrings& GetStrings(Locale locale = Locale::EN);

} // namespace i18n
