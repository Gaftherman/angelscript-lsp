#pragma once

#include <string>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::i18n
{
    class I18n
    {
    public:
        /**
         * @brief Builds the message table for a locale.
         * @param localeTag Locale tag in any BCP 47 spelling - "es", "es-ES", "es_MX", "ES". Only
         *        the primary language subtag selects the table; the region is ignored.
         */
        explicit I18n(const std::string &localeTag = "en");
        ~I18n() = default;

        std::string GetMessage(const std::string &key) const;

    private:
        std::string m_locale;
        ankerl::unordered_dense::map<std::string, std::string> m_messages;
    };
}