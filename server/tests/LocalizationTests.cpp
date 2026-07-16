#include <doctest/doctest.h>
#include "i18n/LspStrings.h"

using namespace i18n;

TEST_SUITE("Localization")
{
    TEST_CASE("L1-L4: ParseLocale")
    {
        CHECK(ParseLocale("en") == Locale::EN);
        CHECK(ParseLocale("en-US") == Locale::EN);
        CHECK(ParseLocale("es") == Locale::ES);
        CHECK(ParseLocale("es-419") == Locale::ES);
        CHECK(ParseLocale("de") == Locale::UNKNOWN);
        CHECK(ParseLocale("") == Locale::UNKNOWN);
    }

    TEST_CASE("L5-L6: GetStrings retrieves correct translation")
    {
        const auto& enStrings = GetStrings(Locale::EN);
        CHECK(std::string(enStrings.kindFunction) == "Function");
        CHECK(std::string(enStrings.hoverIn) == "in");
        
        const auto& esStrings = GetStrings(Locale::ES);
        CHECK(std::string(esStrings.kindFunction) == "Función");
        CHECK(std::string(esStrings.hoverIn) == "en");
        
        // UNKNOWN falls back to EN
        const auto& unknownStrings = GetStrings(Locale::UNKNOWN);
        CHECK(std::string(unknownStrings.kindFunction) == "Function");
    }
}
