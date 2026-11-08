#pragma once

#include <span>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::i18n
{
/**
 * @brief Immutable message table for one locale; safe for concurrent reads.
 */
class I18n
{
  public:
    /**
     * @brief Builds the message table for a locale.
     * @param[in] localeTag Locale tag in any BCP 47 spelling - "es", "es-ES", "es_MX", "ES". Only
     *        the primary language subtag selects the table; the region is ignored.
     */
    explicit I18n(std::string_view localeTag = "en");
    ~I18n() = default;

    /**
     * @brief Retrieves a zero-copy view of a message by key from the message dictionary.
     * @param[in] key Unique message key view.
     * @return The translated message view, or empty string_view if not found.
     */
    [[nodiscard]] std::string_view GetMessageView(std::string_view key) const noexcept;

  private:
    /**
     * @brief Sorted (key, text) span into the generated static locale table, searched by binary search.
     */
    std::span<const std::pair<std::string_view, std::string_view>> m_messages;
};

/**
 * @brief Formats a localized message from the I18n dictionary, substituting args into '{}'.
 *        Falls back to fallbackPattern if key is not found or i18n is null.
 * @tparam Args Formatting argument types.
 * @param[in] i18n Optional pointer to I18n instance.
 * @param[in] key Message key in the translation dictionary.
 * @param[in] fallbackPattern Fallback pattern with '{}' placeholders if key is missing or i18n is null.
 * @param[in] args Formatting arguments to substitute into '{}' placeholders.
 * @return Formatted localized string.
 */
template <typename... Args>
inline std::string FormatMessage(const I18n* i18n, std::string_view key, std::string_view fallbackPattern,
                                 Args&&... args)
{
    std::string_view pattern = (i18n != nullptr) ? i18n->GetMessageView(key) : std::string_view{};
    if (pattern.empty())
    {
        pattern = fallbackPattern;
    }
    if constexpr (sizeof...(Args) == 0)
    {
        return std::string(pattern);
    }
    else
    {
        return fmt::format(fmt::runtime(pattern), std::forward<Args>(args)...);
    }
}

} // namespace angel_lsp::i18n
