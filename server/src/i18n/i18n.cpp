#include "i18n.h"

#include "I18nLocaleData.inc"

#include <algorithm>
#include <cctype>

namespace angel_lsp::i18n
{
/**
 * @brief Reduces a locale tag to its lowercased primary language subtag.
 *        The tag arrives from two places and neither is under this server's control: the client's
 */
static std::string PrimaryLanguageSubtag(const std::string& locale)
{
    std::string language;
    for (const char c : locale)
    {
        if (c == '-' || c == '_')
        {
            break;
        }
        language.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return language;
}

I18n::I18n(const std::string& localeTag)
    : m_locale(PrimaryLanguageSubtag(localeTag)), m_messages(m_locale == "es" ? k_esMessages : k_enMessages)
{
}

std::string_view I18n::GetMessageView(std::string_view key) const noexcept
{
    const auto entry =
        std::lower_bound(m_messages.begin(), m_messages.end(), key,
                         [](const auto& message, std::string_view lookup) { return message.first < lookup; });
    return entry != m_messages.end() && entry->first == key ? entry->second : std::string_view{};
}

std::string I18n::GetMessage(std::string_view key) const
{
    return std::string(GetMessageView(key));
}

std::string I18n::GetMessageOrDefault(std::string_view key, const std::string& defaultMessage) const
{
    const std::string_view view = GetMessageView(key);
    if (!view.empty())
    {
        return std::string(view);
    }
    return defaultMessage;
}
} // namespace angel_lsp::i18n
