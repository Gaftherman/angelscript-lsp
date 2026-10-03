#pragma once

#include <ankerl/unordered_dense.h>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::i18n
{
class I18n
{
  public:
    /**
     * @brief Builds the message table for a locale.
     * @param[in] localeTag Locale tag in any BCP 47 spelling - "es", "es-ES", "es_MX", "ES". Only
     *        the primary language subtag selects the table; the region is ignored.
     */
    explicit I18n(const std::string& localeTag = "en");
    ~I18n() = default;

    /**
     * @brief Retrieves a message by key from the message dictionary.
     * @param[in] key Unique message key.
     * @return The translated message, or empty string if not found.
     */
    [[nodiscard]] std::string GetMessage(const std::string& key) const;

    /**
     * @brief Retrieves a message by key, falling back to a default message if not found.
     * @param[in] key Unique message key.
     * @param[in] defaultMessage Fallback message string.
     * @return The translated message, or defaultMessage if not found or empty.
     */
    [[nodiscard]] std::string GetMessageOrDefault(const std::string& key, const std::string& defaultMessage) const;

    /**
     * @brief Returns the primary language subtag (e.g. "en", "es").
     * @return Lowercased language subtag string.
     */
    [[nodiscard]] const std::string& GetLocale() const noexcept
    {
        return m_locale;
    }

  private:
    std::string m_locale;
    ankerl::unordered_dense::map<std::string, std::string> m_messages;
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
inline std::string FormatMessage(const I18n* i18n, const std::string& key, std::string_view fallbackPattern,
                                 Args&&... args)
{
    std::string pattern;
    if (i18n != nullptr)
    {
        pattern = i18n->GetMessage(key);
    }
    if (pattern.empty())
    {
        pattern = std::string(fallbackPattern);
    }
    if constexpr (sizeof...(Args) == 0)
    {
        return pattern;
    }
    else
    {
        return fmt::format(fmt::runtime(pattern), std::forward<Args>(args)...);
    }
}

} // namespace angel_lsp::i18n