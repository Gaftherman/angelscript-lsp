#include <doctest/doctest.h>

#include "i18n/i18n.h"

#include <string>

using angel_lsp::i18n::I18n;

// =====================================================================================
// Locale selection
// =====================================================================================

TEST_CASE("I18n - A region-qualified tag selects the language")
{
    // The tag arrives from the client's initialize params, where BCP 47 is the rule, and from
    // --locale, which this project's own README documents as `es-ES`. Comparing the whole tag sent
    // every one of these to English.
    const std::string spanish = I18n("es").GetMessage("as-err-void-variable");

    CHECK(I18n("es-ES").GetMessage("as-err-void-variable") == spanish);
    CHECK(I18n("es-419").GetMessage("as-err-void-variable") == spanish);
    CHECK(I18n("es_MX").GetMessage("as-err-void-variable") == spanish);
    CHECK(I18n("ES").GetMessage("as-err-void-variable") == spanish);
}

TEST_CASE("I18n - Spanish and English differ, so the check above means something")
{
    CHECK(I18n("es").GetMessage("as-err-void-variable") !=
          I18n("en").GetMessage("as-err-void-variable"));
}

TEST_CASE("I18n - An unknown language falls back to English")
{
    const std::string english = I18n("en").GetMessage("as-err-void-variable");

    CHECK(I18n("fr-FR").GetMessage("as-err-void-variable") == english);
    CHECK(I18n("").GetMessage("as-err-void-variable") == english);
}

// =====================================================================================
// Coverage
// =====================================================================================

TEST_CASE("I18n - Every code the analyzer emits has both languages")
{
    // Guards the gap this file was written for: a code added to the English table and forgotten in
    // the Spanish one falls back silently, so a Spanish user sees a mixed-language problems panel
    // with nothing to indicate anything is wrong.
    static const std::vector<std::string> k_codes = {
        "as-err-declaration-missing-body", "as-err-external-not-shared",
        "as-err-not-all-paths-return",
        "as-err-break-outside-loop", "as-err-continue-outside-loop",
        "as-err-invalid-case-type", "as-err-duplicate-case-value", "as-err-default-must-be-last",
        "as-err-class-member-const", "as-err-missing-body", "as-err-out-param-default",
        "as-err-opcmp-return-int", "as-err-opequals-return-bool", "as-err-op-overload-global",
        "as-err-binary-operator-arity", "as-err-opindex-no-params", "as-err-const-out-param",
        "as-err-mixin-final", "as-err-mixin-abstract", "as-err-inherit-final",
        "as-err-duplicate-symbol"
    };

    const I18n english("en");
    const I18n spanish("es");

    for (const auto &code : k_codes)
    {
        CAPTURE(code);
        const std::string en = english.GetMessage(code);
        const std::string es = spanish.GetMessage(code);

        CHECK_FALSE(en.empty());
        CHECK_FALSE(es.empty());
        CHECK(en != es);
    }
}
