#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
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
 * @brief Storage for composed accessor names that needs no heap allocation for ordinary identifiers.
 *
 * Names that do not fit the inline storage fall back to a heap string, so a long property name is
 * looked up like any other. A returned view stays valid until the next Compose on the same buffer.
 */
class AccessorNameBuffer
{
  public:
    /**
     * @brief Writes `<prefix><name>` into this buffer.
     * @param[in] prefix Accessor prefix, normally k_getterPrefix or k_setterPrefix.
     * @param[in] name Property name to prefix.
     * @return View of the composed name; never truncated.
     */
    [[nodiscard]] std::string_view Compose(std::string_view prefix, std::string_view name)
    {
        const size_t size = prefix.size() + name.size();
        char* out = m_inline;
        if (size >= k_inlineSize)
        {
            m_heap.resize(size);
            out = m_heap.data();
        }
        std::copy(prefix.begin(), prefix.end(), out);
        std::copy(name.begin(), name.end(), out + prefix.size());
        return std::string_view(out, size);
    }

  private:
    static constexpr size_t k_inlineSize = 128;
    char m_inline[k_inlineSize];
    std::string m_heap;
};

/**
 * @brief Composes `<prefix><name>` into a caller-owned buffer.
 * @param[in] prefix Accessor prefix, normally k_getterPrefix or k_setterPrefix.
 * @param[in] name Property name to prefix.
 * @param[out] buffer Storage that receives the composed name.
 * @return View of the composed name inside buffer; never truncated.
 */
[[nodiscard]] inline std::string_view ComposeAccessorName(std::string_view prefix, std::string_view name,
                                                          AccessorNameBuffer& buffer)
{
    return buffer.Compose(prefix, name);
}

/**
 * @brief Composes `get_<name>` into a caller-owned buffer.
 * @param[in] name Property name to prefix.
 * @param[out] buffer Storage that receives the composed name.
 * @return View of the composed name inside buffer; never truncated.
 */
[[nodiscard]] inline std::string_view ComposeGetterName(std::string_view name, AccessorNameBuffer& buffer)
{
    return buffer.Compose(k_getterPrefix, name);
}

} // namespace angel_lsp::utils
