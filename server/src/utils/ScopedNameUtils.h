#pragma once

#include <string_view>
#include <utility>

namespace angel_lsp::utils
{

/**
 * @brief Strips a leading global scope qualifier ("::") if present.
 * @param[in] name Identifier or scoped name.
 * @return View with leading "::" removed, or original view if absent.
 */
[[nodiscard]] constexpr std::string_view StripGlobalPrefix(std::string_view name) noexcept
{
    return name.starts_with("::") ? name.substr(2) : name;
}

/**
 * @brief Checks if a name begins with the explicit global scope resolution operator ("::").
 * @param[in] name Identifier or scoped name.
 * @return True if name starts with "::".
 */
[[nodiscard]] constexpr bool HasLeadingGlobalPrefix(std::string_view name) noexcept
{
    return name.starts_with("::");
}

/**
 * @brief Returns the enclosing parent scope view by stripping the last "::" segment.
 * @param[in] scope Fully or partially qualified container scope (e.g. "Outer::Inner").
 * @return View to the parent scope ("Outer"), or empty view if no parent separator exists.
 */
[[nodiscard]] constexpr std::string_view ParentScopeView(std::string_view scope) noexcept
{
    const size_t sep = scope.rfind("::");
    return sep == std::string_view::npos ? std::string_view{} : scope.substr(0, sep);
}

/**
 * @brief Splits a scoped name into its qualifier prefix and leaf symbol name.
 * @param[in] full Full scoped path (e.g. "Namespace::Class::Method").
 * @return Pair containing {qualifier ("Namespace::Class"), leaf ("Method")}.
 *         If no "::" exists, qualifier is empty and leaf is the full name.
 */
[[nodiscard]] constexpr std::pair<std::string_view, std::string_view> SplitScopedName(std::string_view full) noexcept
{
    const size_t sep = full.rfind("::");
    if (sep == std::string_view::npos)
    {
        return {std::string_view{}, full};
    }
    return {full.substr(0, sep), full.substr(sep + 2)};
}

/**
 * @brief Traverses each enclosing parent scope from containerScope upwards with zero heap allocations.
 * @tparam Callback Callable with signature void(std::string_view) or bool(std::string_view).
 * @param[in] containerScope Initial container scope.
 * @param[in] cb Callback invoked for each scope level.
 */
template <typename Callback> constexpr void ForEachEnclosingScope(std::string_view containerScope, Callback&& cb)
{
    std::string_view cur = containerScope;
    while (!cur.empty())
    {
        if constexpr (std::is_invocable_r_v<bool, Callback, std::string_view>)
        {
            if (cb(cur))
            {
                break;
            }
        }
        else
        {
            cb(cur);
        }
        cur = ParentScopeView(cur);
    }
}

/**
 * @brief Checks whether two symbol names refer to equivalent global or scoped references.
 * @param[in] lhs First symbol name.
 * @param[in] rhs Second symbol name.
 * @param[in] activeScope Optional currently active container scope context.
 * @return True if both references resolve to equivalent symbol entities.
 */
[[nodiscard]] constexpr bool AreEquivalentGlobalOrScopedNames(std::string_view lhs, std::string_view rhs,
                                                              std::string_view activeScope = {}) noexcept
{
    const std::string_view cleanLhs = StripGlobalPrefix(lhs);
    const std::string_view cleanRhs = StripGlobalPrefix(rhs);
    if (cleanLhs == cleanRhs)
    {
        return true;
    }
    if (!activeScope.empty())
    {
        if (cleanLhs.starts_with(activeScope) && cleanLhs.size() > activeScope.size() + 2 &&
            cleanLhs.substr(activeScope.size(), 2) == "::")
        {
            if (cleanLhs.substr(activeScope.size() + 2) == cleanRhs)
            {
                return true;
            }
        }
        if (cleanRhs.starts_with(activeScope) && cleanRhs.size() > activeScope.size() + 2 &&
            cleanRhs.substr(activeScope.size(), 2) == "::")
        {
            if (cleanRhs.substr(activeScope.size() + 2) == cleanLhs)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace angel_lsp::utils
