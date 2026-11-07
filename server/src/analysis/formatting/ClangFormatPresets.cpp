#include "analysis/formatting/ClangFormatPresets.h"

#include "analysis/formatting/ClangFormatReader.h"

#include <charconv>

namespace angel_lsp::analysis::formatting
{
static void SetPresetStyle(ClangFormatStyle& style, uint32_t indent, BraceStyle brace, PointerAlignment ptr)
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

static void ApplyClangIndentationAndBraces(FormatCodeOptions& options, const ClangFormatStyle& style)
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

static void ApplyClangSpacingAndShortBlocks(FormatCodeOptions& options, const ClangFormatStyle& style)
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

static void ApplyClangBlockAndLimits(FormatCodeOptions& options, const ClangFormatStyle& style)
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

void ApplyClangFormatStyle(FormatCodeOptions& options, const ClangFormatStyle& style)
{
    ApplyClangIndentationAndBraces(options, style);
    ApplyClangSpacingAndShortBlocks(options, style);
    ApplyClangBlockAndLimits(options, style);
}
} // namespace angel_lsp::analysis::formatting
