#pragma once

#include "analysis/index/SymbolTable.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Removes leading and trailing whitespace from a type name view in place.
 * @param[in,out] typeName String view to trim.
 */
constexpr void TrimTypeWhitespace(std::string_view& typeName) noexcept
{
    while (!typeName.empty() && (typeName.front() == ' ' || typeName.front() == '\t'))
    {
        typeName.remove_prefix(1);
    }
    while (!typeName.empty() && (typeName.back() == ' ' || typeName.back() == '\t'))
    {
        typeName.remove_suffix(1);
    }
}

/**
 * @brief Strips a leading "const " prefix from a type name view if present.
 * @param[in] typeName String view to strip.
 * @return String view without leading const.
 */
[[nodiscard]] constexpr std::string_view StripLeadingConst(std::string_view typeName) noexcept
{
    TrimTypeWhitespace(typeName);
    if (typeName.starts_with("const "))
    {
        typeName.remove_prefix(6);
    }
    TrimTypeWhitespace(typeName);
    return typeName;
}

/**
 * @brief Returns the character length of a trailing reference qualifier (&, &in, &out, &inout).
 * @param[in] s String view to inspect.
 * @return Byte length of trailing reference modifier or 0 if none.
 */
[[nodiscard]] constexpr size_t TrailingReferenceModifierLength(std::string_view s) noexcept
{
    if (s.ends_with("& inout"))
    {
        return 7;
    }
    if (s.ends_with("&inout"))
    {
        return 6;
    }
    if (s.ends_with("& out") || s.ends_with("& in"))
    {
        return 5;
    }
    if (s.ends_with("&out"))
    {
        return 4;
    }
    if (s.ends_with("&in"))
    {
        return 3;
    }
    if (!s.empty() && s.back() == '&')
    {
        return 1;
    }
    return 0;
}

/**
 * @brief Returns the character length of a trailing qualifier (const or reference modifier).
 * @param[in] s String view to inspect.
 * @return Byte length of trailing qualifier or 0 if none.
 */
[[nodiscard]] constexpr size_t TrailingQualifierLength(std::string_view s) noexcept
{
    if (s.ends_with(" const") || s.ends_with("\tconst"))
    {
        return 6;
    }
    return TrailingReferenceModifierLength(s);
}

/**
 * @brief Strips trailing reference '&', handle '@', const, and optional array '[]' decorators from a string view.
 * @param[in] result String view to strip.
 * @param[in] stripArrays Whether to also strip trailing array brackets '[]' (defaults to false).
 * @return String view without trailing decorations.
 */
[[nodiscard]] constexpr std::string_view StripTrailingDecorationsView(std::string_view result,
                                                                      bool stripArrays = false) noexcept
{
    bool modified = true;
    while (modified)
    {
        modified = false;
        while (!result.empty() &&
               (result.back() == '@' || result.back() == '&' || result.back() == ' ' || result.back() == '\t'))
        {
            result.remove_suffix(1);
            modified = true;
        }
        if (const size_t len = TrailingQualifierLength(result); len > 0)
        {
            result.remove_suffix(len);
            modified = true;
        }
        else if (stripArrays && result.ends_with("[]"))
        {
            result.remove_suffix(2);
            modified = true;
        }
    }
    return result;
}

/**
 * @brief Checks if a type string starts with a leading const qualifier (e.g. "const Type" or "const Type@").
 * @param[in] type Type string view.
 * @return True if the type begins with "const " or is "const".
 */
[[nodiscard]] constexpr bool HasLeadingConst(std::string_view type) noexcept
{
    TrimTypeWhitespace(type);
    return type == "const" || type.starts_with("const ") || type.starts_with("const\t");
}

/**
 * @brief Checks if a type string ends with a trailing const qualifier (e.g. "Type@ const" or "Type@ const &").
 * @param[in] type Type string view.
 * @return True if the type ends with " const" or "\tconst" before any reference decorators.
 */
[[nodiscard]] constexpr bool HasTrailingConst(std::string_view type) noexcept
{
    TrimTypeWhitespace(type);
    if (const size_t refLen = TrailingReferenceModifierLength(type); refLen > 0)
    {
        type.remove_suffix(refLen);
    }
    TrimTypeWhitespace(type);
    return type == "const" || type.ends_with(" const") || type.ends_with("\tconst");
}

/**
 * @brief Checks if a type string has a const qualifier.
 * @param[in] type Type string view.
 * @return True if the type begins with "const " or ends with " const" or is "const".
 */
[[nodiscard]] constexpr bool IsConstTypeView(std::string_view type) noexcept
{
    return HasLeadingConst(type) || HasTrailingConst(type);
}

/**
 * @brief Checks if a type string represents an AngelScript handle (@) type.
 * @param[in] type Type string view.
 * @return True if the type has a handle decoration on the outermost type.
 */
[[nodiscard]] constexpr bool IsHandleTypeView(std::string_view type) noexcept
{
    TrimTypeWhitespace(type);
    if (type.empty())
    {
        return false;
    }
    if (const size_t refLen = TrailingReferenceModifierLength(type); refLen > 0)
    {
        type.remove_suffix(refLen);
    }
    TrimTypeWhitespace(type);
    if (type.empty())
    {
        return false;
    }
    if (type.ends_with(" const") || type.ends_with("\tconst"))
    {
        type.remove_suffix(6);
        TrimTypeWhitespace(type);
    }
    else if (type == "const")
    {
        return false;
    }
    return !type.empty() && type.back() == '@';
}

/**
 * @brief Checks if a type string represents a const handle (e.g. "const Foo@" or "Foo@ const").
 * @param[in] type Type string view.
 * @return True if both const and handle decorators are present on the outermost type.
 */
[[nodiscard]] constexpr bool IsConstHandleTypeView(std::string_view type) noexcept
{
    return IsHandleTypeView(type) && IsConstTypeView(type);
}

/**
 * @brief Checks if a type string represents a reference (&, &in, &out, &inout) type.
 * @param[in] type Type string view.
 * @return True if the type ends with a reference qualifier.
 */
[[nodiscard]] constexpr bool IsReferenceTypeView(std::string_view type) noexcept
{
    TrimTypeWhitespace(type);
    if (type.empty())
    {
        return false;
    }
    return type.back() == '&' || type.ends_with("&in") || type.ends_with("&out") || type.ends_with("&inout") ||
           type.ends_with("& in") || type.ends_with("& out") || type.ends_with("& inout");
}

/**
 * @brief Checks if a call argument string view is prefixed with an out or inout reference qualifier.
 * @param[in] arg Call argument expression string view.
 * @return True if prefixed with an out or inout reference modifier.
 */
[[nodiscard]] constexpr bool IsOutOrInOutArgumentView(std::string_view arg) noexcept
{
    TrimTypeWhitespace(arg);
    return arg.starts_with("&out ") || arg.starts_with("&inout ") || arg.starts_with("& out ") ||
           arg.starts_with("& inout ") || arg.starts_with("out ") || arg.starts_with("inout ");
}

/**
 * @brief Extracts all type qualifiers (const, handle, reference, etc.) from a type string without heap allocation.
 * @param[in] type Type string view.
 * @return TypeQualifiers bitfield struct.
 */
[[nodiscard]] constexpr TypeQualifiers ExtractTypeQualifiersView(std::string_view type) noexcept
{
    TypeQualifiers q{};
    TrimTypeWhitespace(type);
    if (type.empty())
    {
        return q;
    }
    q.isAuto = (type == "auto");
    q.isConst = IsConstTypeView(type);
    q.isHandle = IsHandleTypeView(type);
    q.isConstHandle = q.isConst && q.isHandle;
    q.isReference = IsReferenceTypeView(type);
    q.isOutRef = (type.ends_with("&out") || type.ends_with("& out"));
    q.isInOutRef = (type.ends_with("&inout") || type.ends_with("& inout"));
    q.isStandaloneRef =
        q.isReference && !q.isOutRef && !q.isInOutRef && !type.ends_with("&in") && !type.ends_with("& in");
    return q;
}

/**
 * @brief Strips trailing reference '&', handle '@', const, and array '[]' decorators.
 * @param[in,out] result String to strip in place.
 */
void StripTrailingDecorations(std::string& result);

/**
 * @brief Reduces a type name view to its innermost base/element type view without allocations.
 * @param[in] typeName The type name view to clean.
 * @param[in] arrayTypeName Optional custom array template name (defaults to "array").
 * @return Cleaned base type name view.
 */
[[nodiscard]] std::string_view CleanBaseTypeView(std::string_view typeName,
                                                 std::string_view arrayTypeName = "") noexcept;

/**
 * @brief Reduces a type name to its innermost base/element type.
 * @param[in] typeName The type name to clean.
 * @param[in] arrayTypeName Optional custom array template name (defaults to "array").
 * @return Cleaned base type name.
 */
std::string CleanBaseType(std::string_view typeName, std::string_view arrayTypeName = "");

/**
 * @brief Canonicalizes array shorthand syntax (e.g., `int[]`, `int[][]`) into template form.
 * @param[in] typeName Type name with array syntax.
 * @param[in] arrayTypeName Array template name (defaults to "array").
 * @return Canonical template representation (e.g. `array<int>`).
 */
std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName);

/**
 * @brief Determines the container/owner type for member lookup on a type.
 * @param[in] typeName Type name to inspect.
 * @param[in] arrayTypeName Array template name.
 * @return Name of the type owning member declarations.
 */
std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName);

/**
 * @brief Normalizes primitive type aliases to canonical keywords (e.g., `int32` -> `int`).
 * @param[in] typeName Type name to canonicalize.
 * @return Canonical type name.
 */
[[nodiscard]] std::string CanonicalizeType(std::string_view typeName);

/**
 * @brief Decomposition of a template type into container name and inner argument view.
 */
struct TemplateDecomposition
{
    std::string_view containerName;
    std::string_view innerArguments;
    bool isTemplate = false;
};

/**
 * @brief Decomposes a template type string into its container name and inner argument substring without allocations.
 * @param[in] typeName The type string (e.g. "array<int>").
 * @return Decomposed template views.
 */
[[nodiscard]] constexpr TemplateDecomposition DecomposeTemplateType(std::string_view typeName) noexcept
{
    if (typeName.ends_with('>'))
    {
        const size_t open = typeName.find('<');
        if (open != std::string_view::npos && open > 0)
        {
            return {
                .containerName = typeName.substr(0, open),
                .innerArguments = typeName.substr(open + 1, typeName.size() - open - 2),
                .isTemplate = true,
            };
        }
    }
    return {
        .containerName = typeName,
        .innerArguments = {},
        .isTemplate = false,
    };
}

/**
 * @brief Splits comma-separated template arguments respecting nested angle brackets.
 * @param[in] inner Inner text between outermost `<` and `>`.
 * @return Vector of individual template argument strings.
 */
std::vector<std::string> SplitTemplateArguments(std::string_view inner);

/**
 * @brief Checks if an identifier or type name contains a scope resolution qualifier (`::`).
 * @param[in] name The identifier or type name to check.
 * @return True if `::` is present.
 */
bool HasScopeQualifier(std::string_view name) noexcept;

/**
 * @brief Splits a scope-qualified name into constituent segments by `::`.
 * @param[in] name The qualified name to split.
 * @return Vector of scope segments.
 */
std::vector<std::string> SplitScopeSegments(std::string_view name);

} // namespace angel_lsp::analysis
