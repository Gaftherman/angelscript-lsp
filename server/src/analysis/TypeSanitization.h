#pragma once

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
void TrimTypeWhitespace(std::string_view& typeName);

/**
 * @brief Strips a leading "const " prefix from a type name view if present.
 * @param[in,out] typeName String view to strip.
 */
void StripLeadingConst(std::string_view& typeName);

/**
 * @brief Strips trailing reference '&', handle '@', const, and array '[]' decorators from a string view in place.
 * @param[in,out] result String view to strip in place.
 */
void StripTrailingDecorationsView(std::string_view& result) noexcept;

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
std::string CanonicalizeArrayType(std::string_view typeName, std::string_view arrayTypeName = "array");

/**
 * @brief Determines the container/owner type for member lookup on a type.
 * @param[in] typeName Type name to inspect.
 * @param[in] arrayTypeName Array template name.
 * @return Name of the type owning member declarations.
 */
std::string MemberOwnerType(std::string_view typeName, std::string_view arrayTypeName = "array");

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
