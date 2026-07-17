#pragma once
#include <string>
#include "i18n/LspStrings.h"

namespace i18n {
    class DiagnosticI18n {
    public:
        // Translates an AngelScript diagnostic message based on the given locale
        static std::string Translate(const std::string& originalMsg, Locale locale);
    };
}
