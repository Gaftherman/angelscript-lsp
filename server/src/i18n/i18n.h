#pragma once

#include <ankerl/unordered_dense.h>
#include <string>

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
    explicit I18n(const std::string& localeTag = "en");
    ~I18n() = default;

    std::string GetMessage(const std::string& key) const;

    /**
     * @brief Returns the primary language subtag (e.g. "en", "es").
     * @return Lowercased language subtag string.
     */
    [[nodiscard]] const std::string& GetLocale() const noexcept
    {
        return m_locale;
    }

    /**
     * @brief Checks if current locale is Spanish ("es").
     * @return True if Spanish language is active.
     */
    [[nodiscard]] bool IsSpanish() const noexcept
    {
        return m_locale == "es";
    }

  private:
    std::string m_locale;
    ankerl::unordered_dense::map<std::string, std::string> m_messages;
};
} // namespace angel_lsp::i18n