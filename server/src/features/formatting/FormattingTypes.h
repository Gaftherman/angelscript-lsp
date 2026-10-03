#pragma once

#include <cstdint>
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Where a block's opening brace goes.
 */
enum class BraceStyle
{
    Allman,      ///< '{' on its own line, aligned with the statement that owns it.
    KAndR,       ///< '{' at the end of the statement line (Attach), and 'else' beside '}'.
    GNU,         ///< '{' on its own line, indented by 2 additional spaces.
    Linux,       ///< Attach for control statements, new line for functions/classes.
    Mozilla,     ///< Attach for classes/functions, new line for enums/records.
    Stroustrup,  ///< Attach for '{', but 'else' placed on its own new line.
    WebKit,      ///< Attach for control statements, new line for functions.
    Whitesmiths, ///< '{' on its own line, indented with the block body.
    Custom       ///< Defined by detailed BraceWrapping options.
};

/**
 * @brief Where the handle symbol '@' (or pointer '*') attaches in declarations.
 */
enum class PointerAlignment
{
    Left,  ///< Attach to type: `Type@ var`. Canonical AngelScript convention.
    Right, ///< Attach to identifier: `Type @var`.
    Middle ///< Centered with spaces: `Type @ var`.
};

/**
 * @brief Where reference symbol '&' attaches in declarations.
 */
enum class ReferenceAlignment
{
    Left,   ///< Attach to type: `Type& ref`.
    Right,  ///< Attach to identifier: `Type &ref`.
    Middle, ///< Centered with spaces: `Type & ref`.
    Pointer ///< Mirrors the configured PointerAlignment.
};

/**
 * @brief Controls spacing before opening parentheses.
 */
enum class SpaceBeforeParensStyle
{
    Never,              ///< Never insert space before '(': `if(c)`, `func(x)`.
    ControlStatements,  ///< Space before control statements: `if (c)`, `while (c)`, but `func(x)`.
    Always,             ///< Space before all parentheses: `if (c)`, `func (x)`.
    NonEmptyParentheses ///< Space only before non-empty parentheses.
};

/**
 * @brief Tab usage style for indentation.
 */
enum class UseTabStyle
{
    Never,                        ///< Always use spaces for indentation.
    Always,                       ///< Always use tabs for indentation.
    ForIndentation,               ///< Use tabs for block indent, spaces for line alignment.
    ForContinuationAndIndentation ///< Use tabs for indent and continuation lines.
};

/**
 * @brief Control for formatting short blocks on a single line.
 */
enum class ShortBlockStyle
{
    Never, ///< Never fold blocks into a single line.
    Empty, ///< Fold only empty blocks `{}` into a single line.
    Always ///< Fold any short block containing statements onto a single line.
};

/**
 * @brief Control for formatting short functions on a single line.
 */
enum class ShortFunctionStyle
{
    None,   ///< Never fold functions into a single line.
    Inline, ///< Fold short inline member methods.
    Empty,  ///< Fold only empty functions `void f() {}`.
    All     ///< Fold all short functions into a single line.
};

/**
 * @brief Controls indentation within namespaces.
 */
enum class NamespaceIndentationStyle
{
    None, ///< No extra indentation inside namespaces.
    All,  ///< Indent all declarations inside namespaces.
    Inner ///< Indent only declarations inside nested/inner namespaces.
};

/**
 * @brief Granular control over brace wrapping across specific constructs.
 */
struct BraceWrappingOptions
{
    bool afterClass = false;
    bool afterControlStatement = false;
    bool afterEnum = false;
    bool afterFunction = false;
    bool afterNamespace = false;
    bool beforeElse = false;
    bool beforeCatch = false;
    bool splitEmptyFunction = false;
    bool splitEmptyRecord = false;
};

/**
 * @brief Parsed configuration options from .clang-format.
 */
struct ClangFormatStyle
{
    std::optional<std::string> basedOnStyle;
    std::optional<uint32_t> indentWidth;
    std::optional<uint32_t> tabWidth;
    std::optional<UseTabStyle> useTab;
    std::optional<BraceStyle> braceStyle;
    std::optional<BraceWrappingOptions> braceWrapping;
    std::optional<PointerAlignment> pointerAlignment;
    std::optional<ReferenceAlignment> referenceAlignment;
    std::optional<bool> spacesInsideParentheses;
    std::optional<bool> spacesInSquareBrackets;
    std::optional<bool> spacesInAngles;
    std::optional<SpaceBeforeParensStyle> spaceBeforeParens;
    std::optional<bool> spaceAfterCStyleCast;
    std::optional<bool> spaceAfterTemplateKeyword;
    std::optional<bool> spaceBeforeAssignmentOperators;
    std::optional<bool> indentCaseLabels;
    std::optional<int32_t> accessModifierOffset;
    std::optional<NamespaceIndentationStyle> namespaceIndentation;
    std::optional<ShortBlockStyle> allowShortBlocksOnASingleLine;
    std::optional<ShortFunctionStyle> allowShortFunctionsOnASingleLine;
    std::optional<bool> allowShortIfStatementsOnASingleLine;
    std::optional<bool> allowShortLoopsOnASingleLine;
    std::optional<uint32_t> maxEmptyLinesToKeep;
    std::optional<bool> keepEmptyLinesAtTheStartOfBlocks;
    std::optional<uint32_t> spacesBeforeTrailingComments;
    std::optional<uint32_t> columnLimit;
    std::optional<bool> disableFormat;
};

/**
 * @brief Unified options bundle applied during code formatting.
 */
struct FormatCodeOptions
{
    lsp::FormattingOptions options;
    BraceStyle braceStyle = BraceStyle::Allman;
    bool spacesInsideParentheses = false;
    bool keepEmptyBlocksOnSingleLine = false;
    PointerAlignment pointerAlignment = PointerAlignment::Left;
    ReferenceAlignment referenceAlignment = ReferenceAlignment::Pointer;
    BraceWrappingOptions braceWrapping;
    bool spacesInSquareBrackets = false;
    bool spacesInAngles = false;
    SpaceBeforeParensStyle spaceBeforeParens = SpaceBeforeParensStyle::ControlStatements;
    bool spaceAfterCStyleCast = false;
    bool spaceAfterTemplateKeyword = false;
    bool spaceBeforeAssignmentOperators = true;
    bool indentCaseLabels = true;
    int32_t accessModifierOffset = -4;
    NamespaceIndentationStyle namespaceIndentation = NamespaceIndentationStyle::All;
    ShortBlockStyle allowShortBlocksOnASingleLine = ShortBlockStyle::Never;
    ShortFunctionStyle allowShortFunctionsOnASingleLine = ShortFunctionStyle::None;
    bool allowShortIfStatementsOnASingleLine = false;
    bool allowShortLoopsOnASingleLine = false;
    uint32_t maxEmptyLinesToKeep = 1;
    bool keepEmptyLinesAtTheStartOfBlocks = false;
    uint32_t spacesBeforeTrailingComments = 1;
    uint32_t columnLimit = 0;
    bool disableFormat = false;
};

/**
 * @brief Context and options for document formatting.
 */
struct FormattingRequest
{
    const std::string& uri;
    const std::string& sourceCode;
    TSTree* tree = nullptr;
    lsp::FormattingOptions options;
    BraceStyle braceStyle = BraceStyle::Allman;
    bool spacesInsideParentheses = false;
    bool keepEmptyBlocksOnSingleLine = false;
    PointerAlignment pointerAlignment = PointerAlignment::Left;
};

/**
 * @brief Context and options for range formatting.
 */
struct RangeFormattingRequest
{
    const std::string& uri;
    const std::string& sourceCode;
    TSTree* tree = nullptr;
    lsp::Range range;
    lsp::FormattingOptions options;
    BraceStyle braceStyle = BraceStyle::Allman;
    bool spacesInsideParentheses = false;
    bool keepEmptyBlocksOnSingleLine = false;
    PointerAlignment pointerAlignment = PointerAlignment::Left;
};

/**
 * @brief Context and options for on-type formatting.
 */
struct OnTypeFormattingRequest
{
    const std::string& uri;
    const std::string& sourceCode;
    TSTree* tree = nullptr;
    lsp::Position position;
    std::string ch;
    lsp::FormattingOptions options;
    BraceStyle braceStyle = BraceStyle::Allman;
    bool spacesInsideParentheses = false;
    bool keepEmptyBlocksOnSingleLine = false;
    PointerAlignment pointerAlignment = PointerAlignment::Left;
};

using FormattingResult = std::vector<lsp::TextEdit>;
using DocumentFormattingRequest = FormattingRequest;
using DocumentRangeFormattingRequest = RangeFormattingRequest;
} // namespace angel_lsp::features
