#include "features/formatting/ClangFormatPresets.h"
#include "features/formatting/ClangFormatReader.h"
#include <charconv>

namespace angel_lsp::features
{
namespace
{
void SetPresetStyle(ClangFormatStyle& style, uint32_t indent, BraceStyle brace, PointerAlignment ptr)
{
    style.indentWidth = indent;
    style.tabWidth = 8;
    style.useTab = UseTabStyle::Never;
    style.braceStyle = brace;
    style.pointerAlignment = ptr;
    style.referenceAlignment = ReferenceAlignment::Pointer;
    style.spacesInsideParentheses = false;
    style.spaceBeforeParens = SpaceBeforeParensStyle::ControlStatements;
}

void ApplyClangIndentationAndBraces(FormatCodeOptions& options, const ClangFormatStyle& style)
{
    if (style.indentWidth.has_value())
    {
        options.options.tabSize = *style.indentWidth;
    }
    if (style.useTab.has_value())
    {
        options.options.insertSpaces = (*style.useTab == UseTabStyle::Never);
    }
    if (style.braceStyle.has_value())
    {
        options.braceStyle = *style.braceStyle;
    }
    if (style.braceWrapping.has_value())
    {
        options.braceWrapping = *style.braceWrapping;
    }
    if (style.pointerAlignment.has_value())
    {
        options.pointerAlignment = *style.pointerAlignment;
    }
    if (style.referenceAlignment.has_value())
    {
        options.referenceAlignment = *style.referenceAlignment;
    }
    if (style.indentCaseLabels.has_value())
    {
        options.indentCaseLabels = *style.indentCaseLabels;
    }
    if (style.accessModifierOffset.has_value())
    {
        options.accessModifierOffset = *style.accessModifierOffset;
    }
    if (style.namespaceIndentation.has_value())
    {
        options.namespaceIndentation = *style.namespaceIndentation;
    }
}

void ApplyClangSpacingAndShortBlocks(FormatCodeOptions& options, const ClangFormatStyle& style)
{
    if (style.spacesInsideParentheses.has_value())
    {
        options.spacesInsideParentheses = *style.spacesInsideParentheses;
    }
    if (style.spacesInSquareBrackets.has_value())
    {
        options.spacesInSquareBrackets = *style.spacesInSquareBrackets;
    }
    if (style.spacesInAngles.has_value())
    {
        options.spacesInAngles = *style.spacesInAngles;
    }
    if (style.spaceBeforeParens.has_value())
    {
        options.spaceBeforeParens = *style.spaceBeforeParens;
    }
    if (style.spaceAfterCStyleCast.has_value())
    {
        options.spaceAfterCStyleCast = *style.spaceAfterCStyleCast;
    }
    if (style.spaceAfterTemplateKeyword.has_value())
    {
        options.spaceAfterTemplateKeyword = *style.spaceAfterTemplateKeyword;
    }
    if (style.spaceBeforeAssignmentOperators.has_value())
    {
        options.spaceBeforeAssignmentOperators = *style.spaceBeforeAssignmentOperators;
    }
}

void ApplyClangBlockAndLimits(FormatCodeOptions& options, const ClangFormatStyle& style)
{
    if (style.allowShortBlocksOnASingleLine.has_value())
    {
        options.allowShortBlocksOnASingleLine = *style.allowShortBlocksOnASingleLine;
        options.keepEmptyBlocksOnSingleLine = (*style.allowShortBlocksOnASingleLine != ShortBlockStyle::Never);
    }
    if (style.allowShortFunctionsOnASingleLine.has_value())
    {
        options.allowShortFunctionsOnASingleLine = *style.allowShortFunctionsOnASingleLine;
    }
    if (style.allowShortIfStatementsOnASingleLine.has_value())
    {
        options.allowShortIfStatementsOnASingleLine = *style.allowShortIfStatementsOnASingleLine;
    }
    if (style.allowShortLoopsOnASingleLine.has_value())
    {
        options.allowShortLoopsOnASingleLine = *style.allowShortLoopsOnASingleLine;
    }
    if (style.maxEmptyLinesToKeep.has_value())
    {
        options.maxEmptyLinesToKeep = *style.maxEmptyLinesToKeep;
    }
    if (style.keepEmptyLinesAtTheStartOfBlocks.has_value())
    {
        options.keepEmptyLinesAtTheStartOfBlocks = *style.keepEmptyLinesAtTheStartOfBlocks;
    }
    if (style.spacesBeforeTrailingComments.has_value())
    {
        options.spacesBeforeTrailingComments = *style.spacesBeforeTrailingComments;
    }
    if (style.columnLimit.has_value())
    {
        options.columnLimit = *style.columnLimit;
    }
    if (style.disableFormat.has_value())
    {
        options.disableFormat = *style.disableFormat;
    }
}
} // namespace

void ApplyBasedOnStylePreset(ClangFormatStyle& style, std::string_view preset)
{
    style.basedOnStyle = std::string(preset);
    if (preset == "LLVM" || preset == "llvm")
    {
        SetPresetStyle(style, 2, BraceStyle::KAndR, PointerAlignment::Right);
        style.indentCaseLabels = false;
        style.accessModifierOffset = -2;
        style.columnLimit = 80;
    }
    else if (preset == "Google" || preset == "google" || preset == "Chromium" || preset == "chromium")
    {
        SetPresetStyle(style, 2, BraceStyle::KAndR, PointerAlignment::Left);
        style.indentCaseLabels = true;
        style.accessModifierOffset = -1;
        style.columnLimit = 80;
    }
    else if (preset == "Mozilla" || preset == "mozilla")
    {
        SetPresetStyle(style, 2, BraceStyle::Mozilla, PointerAlignment::Left);
        style.indentCaseLabels = true;
        style.accessModifierOffset = -2;
        style.columnLimit = 80;
    }
    else if (preset == "WebKit" || preset == "webkit")
    {
        SetPresetStyle(style, 4, BraceStyle::WebKit, PointerAlignment::Left);
        style.indentCaseLabels = false;
        style.accessModifierOffset = -4;
        style.namespaceIndentation = NamespaceIndentationStyle::Inner;
    }
    else if (preset == "Microsoft" || preset == "microsoft")
    {
        SetPresetStyle(style, 4, BraceStyle::Allman, PointerAlignment::Left);
        style.indentCaseLabels = false;
        style.accessModifierOffset = -4;
        style.namespaceIndentation = NamespaceIndentationStyle::All;
        style.columnLimit = 120;
    }
    else if (preset == "GNU" || preset == "gnu")
    {
        SetPresetStyle(style, 2, BraceStyle::GNU, PointerAlignment::Right);
        style.spaceBeforeParens = SpaceBeforeParensStyle::Always;
        style.indentCaseLabels = false;
        style.columnLimit = 79;
    }
}

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

std::optional<uint32_t> ParseClangUInt(std::string_view val)
{
    uint32_t result = 0;
    auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
    if (ec == std::errc() && ptr == val.data() + val.size())
    {
        return result;
    }
    return std::nullopt;
}

std::optional<int32_t> ParseClangInt(std::string_view val)
{
    int32_t result = 0;
    auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), result);
    if (ec == std::errc() && ptr == val.data() + val.size())
    {
        return result;
    }
    return std::nullopt;
}

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

void ApplyClangFormatStyle(FormatCodeOptions& options, const ClangFormatStyle& style)
{
    ApplyClangIndentationAndBraces(options, style);
    ApplyClangSpacingAndShortBlocks(options, style);
    ApplyClangBlockAndLimits(options, style);
}
} // namespace angel_lsp::features
