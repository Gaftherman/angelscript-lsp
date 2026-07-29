#pragma once

#include <string>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::i18n
{
    class I18n
    {
    public:
        explicit I18n(const std::string &locale = "en");
        ~I18n() = default;

        std::string GetMessage(const std::string &key) const;

    private:
        std::string m_locale;
        ankerl::unordered_dense::map<std::string, std::string> m_messages;
    };
}