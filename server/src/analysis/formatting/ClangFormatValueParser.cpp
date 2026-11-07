#include "analysis/formatting/ClangFormatPresets.h"

#include <charconv>
#include <optional>
#include <string_view>

namespace angel_lsp::analysis::formatting
{

std::optional<bool> ParseClangBool(std::string_view val)
{
    if (val == "true" || val == "True" || val == "yes" || val == "Yes" || val == "1" || val == "Always" ||
        val == "always")
    {
        return true;
    }
    if (val == "false" || val == "False" || val == "no" || val == "No" || val == "0" || val == "Never" ||
        val == "never")
    {
        return false;
    }
    return std::nullopt;
}

/**
 * @brief Parses an integer value from a string view using std::from_chars.
 * @tparam T Integral type to parse.
 * @param[in] val Input string view.
 * @return Parsed value if valid; std::nullopt otherwise.
 */
template <typename T> std::optional<T> ParseClangInteger(std::string_view val)
{
    T result = 0;
    auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
    if (ec == std::errc() && ptr == val.data() + val.size())
    {
        return result;
    }
    return std::nullopt;
}

/**
 * @brief Parses an unsigned 32-bit integer value from a clang-format config string.
 * @param[in] val String view containing integer literal representation.
 * @return Parsed uint32_t value if valid, or std::nullopt on conversion failure.
 */
std::optional<uint32_t> ParseClangUInt(std::string_view val)
{
    return ParseClangInteger<uint32_t>(val);
}

/**
 * @brief Parses a signed 32-bit integer value from a clang-format config string.
 * @param[in] val String view containing integer literal representation.
 * @return Parsed int32_t value if valid, or std::nullopt on conversion failure.
 */
std::optional<int32_t> ParseClangInt(std::string_view val)
{
    return ParseClangInteger<int32_t>(val);
}

/**
 * @brief Parses a BraceStyle enumeration from a clang-format BreakBeforeBraces string.
 * @param[in] val Configuration string value (e.g. \"Allman\", \"Attach\", \"Linux\").
 * @return Parsed BraceStyle enum value if recognized, or std::nullopt otherwise.
 */
std::optional<BraceStyle> ParseClangBraceStyle(std::string_view val)
{
    if (val.rfind("BS_", 0) == 0)
        val.remove_prefix(3);
    static constexpr std::pair<std::string_view, BraceStyle> kTable[] = {{"Allman", BraceStyle::Allman},
                                                                         {"Attach", BraceStyle::KAndR},
                                                                         {"Java", BraceStyle::KAndR},
                                                                         {"GNU", BraceStyle::GNU},
                                                                         {"Linux", BraceStyle::Linux},
                                                                         {"Mozilla", BraceStyle::Mozilla},
                                                                         {"Stroustrup", BraceStyle::Stroustrup},
                                                                         {"WebKit", BraceStyle::WebKit},
                                                                         {"Webkit", BraceStyle::WebKit},
                                                                         {"Whitesmiths", BraceStyle::Whitesmiths},
                                                                         {"Custom", BraceStyle::Custom}};
    for (const auto& [name, style] : kTable)
    {
        if (val == name)
            return style;
    }
    return std::nullopt;
}

std::optional<PointerAlignment> ParseClangPointerAlignment(std::string_view val)
{
    if (val == "Left" || val == "left" || val == "PAS_Left")
        return PointerAlignment::Left;
    if (val == "Right" || val == "right" || val == "PAS_Right")
        return PointerAlignment::Right;
    if (val == "Middle" || val == "middle" || val == "PAS_Middle")
        return PointerAlignment::Middle;
    return std::nullopt;
}

std::optional<ReferenceAlignment> ParseClangReferenceAlignment(std::string_view val)
{
    if (val == "Left" || val == "left" || val == "RAS_Left")
        return ReferenceAlignment::Left;
    if (val == "Right" || val == "right" || val == "RAS_Right")
        return ReferenceAlignment::Right;
    if (val == "Middle" || val == "middle" || val == "RAS_Middle")
        return ReferenceAlignment::Middle;
    if (val == "Pointer" || val == "pointer" || val == "RAS_Pointer")
        return ReferenceAlignment::Pointer;
    return std::nullopt;
}

/**
 * @brief Parses a SpaceBeforeParensStyle enum value from a clang-format configuration string.
 * @param[in] val Configuration string value.
 * @return Parsed SpaceBeforeParensStyle enum if recognized, or std::nullopt.
 */
std::optional<SpaceBeforeParensStyle> ParseClangSpaceBeforeParens(std::string_view val)
{
    if (val == "Never" || val == "never" || val == "false" || val == "SBPO_Never")
        return SpaceBeforeParensStyle::Never;
    if (val == "ControlStatements" || val == "SBPO_ControlStatements")
        return SpaceBeforeParensStyle::ControlStatements;
    if (val == "Always" || val == "always" || val == "true" || val == "SBPO_Always")
        return SpaceBeforeParensStyle::Always;
    if (val == "NonEmptyParentheses" || val == "SBPO_NonEmptyParentheses")
        return SpaceBeforeParensStyle::NonEmptyParentheses;
    return std::nullopt;
}

/**
 * @brief Parses a UseTabStyle enum value from a clang-format UseTab setting.
 * @param[in] val Configuration string value (e.g. \"Never\", \"Always\", \"ForIndentation\").
 * @return Parsed UseTabStyle enum if recognized, or std::nullopt.
 */
std::optional<UseTabStyle> ParseClangUseTab(std::string_view val)
{
    if (val == "Never" || val == "never" || val == "false" || val == "UT_Never")
        return UseTabStyle::Never;
    if (val == "Always" || val == "always" || val == "true" || val == "UT_Always")
        return UseTabStyle::Always;
    if (val == "ForIndentation" || val == "UT_ForIndentation")
        return UseTabStyle::ForIndentation;
    if (val == "ForContinuationAndIndentation" || val == "UT_ForContinuationAndIndentation")
        return UseTabStyle::ForContinuationAndIndentation;
    return std::nullopt;
}

std::optional<ShortBlockStyle> ParseClangShortBlockStyle(std::string_view val)
{
    if (val.rfind("SBS_", 0) == 0)
        val.remove_prefix(4);
    if (val == "Never" || val == "never" || val == "false" || val == "False")
        return ShortBlockStyle::Never;
    if (val == "Empty" || val == "empty")
        return ShortBlockStyle::Empty;
    if (val == "Always" || val == "always" || val == "true" || val == "True")
        return ShortBlockStyle::Always;
    return std::nullopt;
}

/**
 * @brief Parses a ShortFunctionStyle enum value from an AllowShortFunctionsOnASingleLine setting.
 * @param[in] val Configuration string value.
 * @return Parsed ShortFunctionStyle enum if recognized, or std::nullopt.
 */
std::optional<ShortFunctionStyle> ParseClangShortFunctionStyle(std::string_view val)
{
    if (val.rfind("SFS_", 0) == 0)
        val.remove_prefix(4);
    static constexpr std::pair<std::string_view, ShortFunctionStyle> kTable[] = {
        {"None", ShortFunctionStyle::None},     {"none", ShortFunctionStyle::None},
        {"false", ShortFunctionStyle::None},    {"False", ShortFunctionStyle::None},
        {"Inline", ShortFunctionStyle::Inline}, {"inline", ShortFunctionStyle::Inline},
        {"Empty", ShortFunctionStyle::Empty},   {"empty", ShortFunctionStyle::Empty},
        {"All", ShortFunctionStyle::All},       {"all", ShortFunctionStyle::All},
        {"true", ShortFunctionStyle::All},      {"True", ShortFunctionStyle::All}};
    for (const auto& [name, style] : kTable)
    {
        if (val == name)
            return style;
    }
    return std::nullopt;
}

std::optional<NamespaceIndentationStyle> ParseClangNamespaceIndentation(std::string_view val)
{
    if (val == "None" || val == "none" || val == "NI_None")
        return NamespaceIndentationStyle::None;
    if (val == "All" || val == "all" || val == "NI_All")
        return NamespaceIndentationStyle::All;
    if (val == "Inner" || val == "inner" || val == "NI_Inner")
        return NamespaceIndentationStyle::Inner;
    return std::nullopt;
}
} // namespace angel_lsp::analysis::formatting
