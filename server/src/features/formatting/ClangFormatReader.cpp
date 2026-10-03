#include "features/formatting/ClangFormatReader.h"
#include "features/formatting/ClangFormatPresets.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace angel_lsp::features
{
namespace
{
std::string_view Trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

void ApplyBraceWrappingSubKey(std::string_view key, std::string_view val, BraceWrappingOptions& wrap)
{
    auto b = ParseClangBool(val);
    if (!b)
        return;
    if (key == "AfterClass")
        wrap.afterClass = *b;
    else if (key == "AfterControlStatement")
        wrap.afterControlStatement = *b;
    else if (key == "AfterEnum")
        wrap.afterEnum = *b;
    else if (key == "AfterFunction")
        wrap.afterFunction = *b;
    else if (key == "AfterNamespace")
        wrap.afterNamespace = *b;
    else if (key == "BeforeElse")
        wrap.beforeElse = *b;
    else if (key == "BeforeCatch")
        wrap.beforeCatch = *b;
    else if (key == "SplitEmptyFunction")
        wrap.splitEmptyFunction = *b;
    else if (key == "SplitEmptyRecord")
        wrap.splitEmptyRecord = *b;
}

void ApplyIndentationKey(std::string_view key, std::string_view val, ClangFormatStyle& style)
{
    if (key == "IndentWidth")
        style.indentWidth = ParseClangUInt(val);
    else if (key == "TabWidth")
        style.tabWidth = ParseClangUInt(val);
    else if (key == "UseTab")
        style.useTab = ParseClangUseTab(val);
    else if (key == "IndentCaseLabels")
        style.indentCaseLabels = ParseClangBool(val);
    else if (key == "AccessModifierOffset")
        style.accessModifierOffset = ParseClangInt(val);
    else if (key == "NamespaceIndentation")
        style.namespaceIndentation = ParseClangNamespaceIndentation(val);
}

void ApplyAlignmentAndBraceKey(std::string_view key, std::string_view val, ClangFormatStyle& style)
{
    if (key == "BreakBeforeBraces")
        style.braceStyle = ParseClangBraceStyle(val);
    else if (key == "PointerAlignment")
        style.pointerAlignment = ParseClangPointerAlignment(val);
    else if (key == "ReferenceAlignment")
        style.referenceAlignment = ParseClangReferenceAlignment(val);
    else if (key == "DerivePointerAlignment")
    {
        // Placeholder for compatibility
    }
}

void ApplySpacingKey(std::string_view key, std::string_view val, ClangFormatStyle& style)
{
    if (key == "SpacesInParentheses" || key == "SpacesInParens")
        style.spacesInsideParentheses = ParseClangBool(val);
    else if (key == "SpacesInSquareBrackets")
        style.spacesInSquareBrackets = ParseClangBool(val);
    else if (key == "SpacesInAngles")
        style.spacesInAngles = ParseClangBool(val);
    else if (key == "SpaceBeforeParens")
        style.spaceBeforeParens = ParseClangSpaceBeforeParens(val);
    else if (key == "SpaceAfterCStyleCast")
        style.spaceAfterCStyleCast = ParseClangBool(val);
    else if (key == "SpaceAfterTemplateKeyword")
        style.spaceAfterTemplateKeyword = ParseClangBool(val);
    else if (key == "SpaceBeforeAssignmentOperators")
        style.spaceBeforeAssignmentOperators = ParseClangBool(val);
    else if (key == "SpacesBeforeTrailingComments")
        style.spacesBeforeTrailingComments = ParseClangUInt(val);
}

void ApplyBlockAndLimitsKey(std::string_view key, std::string_view val, ClangFormatStyle& style)
{
    if (key == "AllowShortBlocksOnASingleLine" || key == "KeepEmptyBlocksOnSingleLine")
        style.allowShortBlocksOnASingleLine = ParseClangShortBlockStyle(val);
    else if (key == "AllowShortFunctionsOnASingleLine")
        style.allowShortFunctionsOnASingleLine = ParseClangShortFunctionStyle(val);
    else if (key == "AllowShortIfStatementsOnASingleLine")
        style.allowShortIfStatementsOnASingleLine = ParseClangBool(val);
    else if (key == "AllowShortLoopsOnASingleLine")
        style.allowShortLoopsOnASingleLine = ParseClangBool(val);
    else if (key == "MaxEmptyLinesToKeep")
        style.maxEmptyLinesToKeep = ParseClangUInt(val);
    else if (key == "KeepEmptyLinesAtTheStartOfBlocks")
        style.keepEmptyLinesAtTheStartOfBlocks = ParseClangBool(val);
    else if (key == "ColumnLimit")
        style.columnLimit = ParseClangUInt(val);
    else if (key == "DisableFormat")
        style.disableFormat = ParseClangBool(val);
}

void ParseYamlSection(std::string_view section, ClangFormatStyle& style, std::string& langOut)
{
    size_t pos = 0;
    bool inBraceWrapping = false;
    BraceWrappingOptions wrap;

    while (pos < section.size())
    {
        size_t next = section.find('\n', pos);
        std::string_view line =
            (next == std::string_view::npos) ? section.substr(pos) : section.substr(pos, next - pos);
        pos = (next == std::string_view::npos) ? section.size() : next + 1;

        if (size_t c = line.find('#'); c != std::string_view::npos)
            line = line.substr(0, c);
        line = Trim(line);
        if (line.empty())
            continue;

        size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;

        std::string_view key = Trim(line.substr(0, colon));
        std::string_view val = Trim(line.substr(colon + 1));

        if (key == "Language")
        {
            langOut = std::string(val);
            continue;
        }
        if (key == "BasedOnStyle")
        {
            ApplyBasedOnStylePreset(style, val);
            continue;
        }
        if (key == "BraceWrapping")
        {
            inBraceWrapping = true;
            continue;
        }

        if (inBraceWrapping && (line.front() == ' ' || line.front() == '\t'))
        {
            ApplyBraceWrappingSubKey(key, val, wrap);
            style.braceWrapping = wrap;
            continue;
        }
        inBraceWrapping = false;

        ApplyIndentationKey(key, val, style);
        ApplyAlignmentAndBraceKey(key, val, style);
        ApplySpacingKey(key, val, style);
        ApplyBlockAndLimitsKey(key, val, style);
    }
}

std::string NormalizePath(std::string_view uriOrPath)
{
    std::string path(uriOrPath);
    if (path.rfind("file:///", 0) == 0)
        path = path.substr(8);
    else if (path.rfind("file://", 0) == 0)
        path = path.substr(7);
    return path;
}
} // namespace

std::optional<ClangFormatStyle> ParseClangFormat(std::string_view content)
{
    ClangFormatStyle globalStyle;
    std::optional<ClangFormatStyle> cppStyle;
    std::optional<ClangFormatStyle> asStyle;
    bool hasAny = false;

    size_t docStart = 0;
    while (docStart < content.size())
    {
        size_t docEnd = content.find("\n---", docStart);
        std::string_view section =
            (docEnd == std::string_view::npos) ? content.substr(docStart) : content.substr(docStart, docEnd - docStart);
        docStart = (docEnd == std::string_view::npos) ? content.size() : docEnd + 4;

        ClangFormatStyle current = globalStyle;
        std::string lang;
        ParseYamlSection(section, current, lang);

        if (lang.empty())
        {
            globalStyle = current;
            hasAny = true;
        }
        else if (lang == "Cpp" || lang == "cpp")
        {
            cppStyle = current;
        }
        else if (lang == "AngelScript" || lang == "angelscript")
        {
            asStyle = current;
        }
    }

    if (asStyle)
        return asStyle;
    if (cppStyle)
        return cppStyle;
    if (hasAny)
        return globalStyle;
    return std::nullopt;
}

std::optional<ClangFormatStyle> LoadClangFormatForFile(std::string_view documentUriOrPath)
{
    if (documentUriOrPath.empty())
        return std::nullopt;

    std::string normPath = NormalizePath(documentUriOrPath);
    std::error_code ec;
    std::filesystem::path current(normPath);

    if (!std::filesystem::exists(current, ec))
        return std::nullopt;
    if (std::filesystem::is_regular_file(current, ec))
        current = current.parent_path();

    const std::string_view candidates[] = {".clang-format", "_clang-format", ".as-clang-format"};

    while (!current.empty())
    {
        for (std::string_view candidate : candidates)
        {
            auto candPath = current / candidate;
            if (std::filesystem::exists(candPath, ec) && std::filesystem::is_regular_file(candPath, ec))
            {
                std::ifstream file(candPath);
                if (file.is_open())
                {
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    return ParseClangFormat(buffer.str());
                }
            }
        }
        if (!current.has_relative_path() || current == current.parent_path())
            break;
        current = current.parent_path();
    }
    return std::nullopt;
}
} // namespace angel_lsp::features
