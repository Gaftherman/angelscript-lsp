#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace angel_lsp::utils
{

/**
 * @brief Standard AngelScript scope resolution operator.
 */
inline constexpr std::string_view k_scopeSeparator = "::";

/**
 * @brief Strips a leading global scope qualifier ("::") if present.
 * @param[in] name Identifier or scoped name.
 * @return View with leading "::" removed, or original view if absent.
 */
[[nodiscard]] constexpr std::string_view StripGlobalPrefix(std::string_view name) noexcept
{
    return name.starts_with(k_scopeSeparator) ? name.substr(k_scopeSeparator.size()) : name;
}

/**
 * @brief Checks if a name begins with the explicit global scope resolution operator ("::").
 * @param[in] name Identifier or scoped name.
 * @return True if name starts with "::".
 */
[[nodiscard]] constexpr bool HasLeadingGlobalPrefix(std::string_view name) noexcept
{
    return name.starts_with(k_scopeSeparator);
}

/**
 * @brief Checks if a name ends with the scope resolution operator ("::").
 * @param[in] name Identifier or scoped name.
 * @return True if name ends with "::".
 */
[[nodiscard]] constexpr bool EndsWithScopeSeparator(std::string_view name) noexcept
{
    return name.ends_with(k_scopeSeparator);
}

/**
 * @brief Strips a trailing scope separator ("::") if present.
 * @param[in] name Identifier or scoped name.
 * @return View with trailing "::" removed, or original view if absent.
 */
[[nodiscard]] constexpr std::string_view StripTrailingScopeSeparator(std::string_view name) noexcept
{
    return EndsWithScopeSeparator(name) ? name.substr(0, name.size() - k_scopeSeparator.size()) : name;
}

/**
 * @brief Checks in O(1) without heap allocation if full ends with "::" + member.
 * @param[in] full Full scoped path or identifier.
 * @param[in] member Member or leaf name to match.
 * @return True if full ends with "::" followed by member.
 */
[[nodiscard]] constexpr bool EndsWithScopedMember(std::string_view full, std::string_view member) noexcept
{
    if (full.size() < member.size() + k_scopeSeparator.size())
    {
        return false;
    }
    if (!full.ends_with(member))
    {
        return false;
    }
    const size_t sepPos = full.size() - member.size() - k_scopeSeparator.size();
    return full.substr(sepPos, k_scopeSeparator.size()) == k_scopeSeparator;
}

/**
 * @brief Checks in O(1) without heap allocation if name starts with prefix + "::".
 * @param[in] name Full scoped name to test.
 * @param[in] prefix Scope prefix to test.
 * @return True if name starts with prefix followed by "::".
 */
[[nodiscard]] constexpr bool HasScopePrefix(std::string_view name, std::string_view prefix) noexcept
{
    if (prefix.empty() || name.size() < prefix.size() + k_scopeSeparator.size())
    {
        return false;
    }
    return name.starts_with(prefix) && name.substr(prefix.size(), k_scopeSeparator.size()) == k_scopeSeparator;
}

/**
 * @brief Concatenates a scope qualifier and a symbol name with "::" in a single heap allocation.
 * @param[in] scope Scope qualifier prefix (e.g. "Namespace").
 * @param[in] name Leaf symbol name (e.g. "Class").
 * @return Qualified scoped name ("Namespace::Class"), or name if scope is empty.
 */
[[nodiscard]] inline std::string JoinScopedName(std::string_view scope, std::string_view name)
{
    if (scope.empty())
    {
        return std::string(name);
    }
    if (name.empty())
    {
        return std::string(scope);
    }
    std::string out;
    out.reserve(scope.size() + k_scopeSeparator.size() + name.size());
    out.append(scope);
    out.append(k_scopeSeparator);
    out.append(name);
    return out;
}

/**
 * @brief Appends a scope-qualified name segment to an existing buffer using "::".
 * @param[in,out] buffer Existing scoped string buffer.
 * @param[in] name Segment to append.
 */
inline void AppendScopedName(std::string& buffer, std::string_view name)
{
    if (name.empty())
    {
        return;
    }
    if (!buffer.empty())
    {
        buffer.append(k_scopeSeparator);
    }
    buffer.append(name);
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
 * @brief Checks if a name contains the scope resolution operator ("::").
 * @param[in] name Identifier or scoped name.
 * @return True if "::" is present.
 */
[[nodiscard]] constexpr bool ContainsScopeSeparator(std::string_view name) noexcept
{
    return name.find("::") != std::string_view::npos;
}

/**
 * @brief Returns the last (unqualified) symbol name segment from a scoped name.
 * @param[in] full Full scoped path (e.g. "Namespace::Class::Method").
 * @return View to the leaf identifier segment ("Method").
 */
[[nodiscard]] constexpr std::string_view LastScopeSegment(std::string_view full) noexcept
{
    return SplitScopedName(full).second;
}

/**
 * @brief Returns the first scope segment before the first "::".
 * @param[in] full Full scoped path (e.g. "Namespace::Class::Method").
 * @return First segment ("Namespace"), or full name if no "::" exists.
 */
[[nodiscard]] constexpr std::string_view FirstScopeSegment(std::string_view full) noexcept
{
    const size_t sep = full.find("::");
    return (sep == std::string_view::npos) ? full : full.substr(0, sep);
}

/**
 * @brief Strips the first scope segment and the first "::", returning the remaining path.
 * @param[in] full Full scoped path (e.g. "Namespace::Class::Method").
 * @return Remaining path ("Class::Method"), or empty view if no "::" exists.
 */
[[nodiscard]] constexpr std::string_view StripFirstScopeSegment(std::string_view full) noexcept
{
    const size_t sep = full.find("::");
    return (sep == std::string_view::npos) ? std::string_view{} : full.substr(sep + 2);
}

/**
 * @brief Splits a scoped name at the first "::" separator.
 * @param[in] full Full scoped path (e.g. "Namespace::Class::Method").
 * @return Pair of {first_segment ("Namespace"), remaining_segments ("Class::Method")}.
 */
[[nodiscard]] constexpr std::pair<std::string_view, std::string_view>
SplitFirstScopeSegment(std::string_view full) noexcept
{
    const size_t sep = full.find("::");
    if (sep == std::string_view::npos)
    {
        return {full, std::string_view{}};
    }
    return {full.substr(0, sep), full.substr(sep + 2)};
}

/**
 * @brief Alias to SplitScopedName for canonical naming.
 */
[[nodiscard]] constexpr std::pair<std::string_view, std::string_view> SplitScopeAndName(std::string_view full) noexcept
{
    return SplitScopedName(full);
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
