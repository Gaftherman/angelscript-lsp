#pragma once

#include <string_view>

namespace angel_lsp::utils
{

/**
 * @brief Name prefix AngelScript uses for virtual property getter methods (`get_x`).
 */
inline constexpr std::string_view k_getterPrefix = "get_";

/**
 * @brief Name prefix AngelScript uses for virtual property setter methods (`set_x`).
 */
inline constexpr std::string_view k_setterPrefix = "set_";

/**
 * @brief Tests whether a method name follows the getter accessor convention.
 * @param[in] name Unqualified method name.
 * @return True when name starts with `get_`, including the bare prefix itself.
 */
[[nodiscard]] constexpr bool IsGetterName(std::string_view name) noexcept
{
    return name.starts_with(k_getterPrefix);
}

/**
 * @brief Tests whether a method name follows the setter accessor convention.
 * @param[in] name Unqualified method name.
 * @return True when name starts with `set_`, including the bare prefix itself.
 */
[[nodiscard]] constexpr bool IsSetterName(std::string_view name) noexcept
{
    return name.starts_with(k_setterPrefix);
}

/**
 * @brief Extracts the virtual property name addressed by a `get_x` or `set_x` accessor.
 * @param[in] name Unqualified method name.
 * @return View of the property part inside name, or an empty view when name is not an accessor
 *         or consists of the bare prefix only.
 */
[[nodiscard]] constexpr std::string_view AccessorPropertyName(std::string_view name) noexcept
{
    for (const std::string_view prefix : {k_getterPrefix, k_setterPrefix})
    {
        if (name.size() > prefix.size() && name.starts_with(prefix))
        {
            return name.substr(prefix.size());
        }
    }
    return {};
}

} // namespace angel_lsp::utils
