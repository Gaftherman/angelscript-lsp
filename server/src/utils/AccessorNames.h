#pragma once

#include <algorithm>
#include <span>
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

/**
 * @brief Writes `<prefix><name>` into a caller-owned buffer, so an accessor lookup needs no heap allocation.
 * @param[in] prefix Accessor prefix, normally k_getterPrefix or k_setterPrefix.
 * @param[in] name Property name to prefix.
 * @param[out] buffer Storage that receives the composed name.
 * @return View of the composed name inside buffer, or an empty view when the name does not fit with room
 *         to spare.
 */
[[nodiscard]] inline std::string_view ComposeAccessorName(std::string_view prefix, std::string_view name,
                                                          std::span<char> buffer) noexcept
{
    if (name.size() + prefix.size() >= buffer.size())
    {
        return {};
    }
    std::copy(prefix.begin(), prefix.end(), buffer.begin());
    std::copy(name.begin(), name.end(), buffer.begin() + prefix.size());
    return std::string_view(buffer.data(), prefix.size() + name.size());
}

/**
 * @brief Writes `get_<name>` into a caller-owned buffer, so a getter lookup needs no heap allocation.
 * @param[in] name Property name to prefix.
 * @param[out] buffer Storage that receives the composed name.
 * @return View of the composed name inside buffer, or an empty view when the name does not fit with room
 *         to spare.
 */
[[nodiscard]] inline std::string_view ComposeGetterName(std::string_view name, std::span<char> buffer) noexcept
{
    return ComposeAccessorName(k_getterPrefix, name, buffer);
}

} // namespace angel_lsp::utils
