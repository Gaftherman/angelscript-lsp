#pragma once

#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>

namespace angel_lsp
{
/**
 * @brief Transparent hash functor enabling std::string_view heterogeneous lookups in hash tables.
 */
struct TransparentStringHash
{
    using is_transparent = void;
    using is_avalanching = void;

    [[nodiscard]] uint64_t operator()(std::string_view sv) const noexcept
    {
        return ankerl::unordered_dense::hash<std::string_view>{}(sv);
    }
};

/**
 * @brief Transparent string set allowing heterogeneous lookups using std::string_view.
 */
using TransparentStringSet = ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>;

} // namespace angel_lsp
