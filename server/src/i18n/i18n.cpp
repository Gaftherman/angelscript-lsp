#include "i18n.h"

#include "I18nLocaleData.inc"

#include <algorithm>
#include <cctype>
#include <span>
#include <string_view>
#include <utility>

namespace angel_lsp::i18n
{
/**
 * @brief Selects the message table from the primary language subtag of a BCP 47 tag, ignoring case and region.
 * @param[in] localeTag Locale tag such as "es", "es-ES", "es_MX" or "ES".
 * @return The Spanish table when the primary subtag is "es"; otherwise the English table.
 */
static std::span<const std::pair<std::string_view, std::string_view>> SelectMessages(std::string_view localeTag)
{
    const std::string_view primary = localeTag.substr(0, localeTag.find_first_of("-_"));
    if (primary.size() == 2 && std::tolower(static_cast<unsigned char>(primary[0])) == static_cast<int>('e') &&
        std::tolower(static_cast<unsigned char>(primary[1])) == static_cast<int>('s'))
    {
        return k_esMessages;
    }
    return k_enMessages;
}

I18n::I18n(std::string_view localeTag) : m_messages(SelectMessages(localeTag))
{
}

std::string_view I18n::GetMessageView(std::string_view key) const noexcept
{
    const auto entry =
        std::lower_bound(m_messages.begin(), m_messages.end(), key,
                         [](const auto& message, std::string_view lookup) { return message.first < lookup; });
    return entry != m_messages.end() && entry->first == key ? entry->second : std::string_view{};
}

} // namespace angel_lsp::i18n
