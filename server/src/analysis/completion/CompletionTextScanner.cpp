#include "analysis/completion/CompletionInternal.h"
#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/SignatureFormatter.h"
#include "analysis/index/CompletionFacts.h"
#include "analysis/resolution/CompletionReceiverResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/AngelScriptParser.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "utils/IncludeResolver.h"
#include "utils/PositionEncoding.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>

namespace angel_lsp::analysis::completion
{

std::string GetLinePrefix(std::string_view sourceCode, uint32_t line, uint32_t character)
{
    size_t currentLine = 0;
    size_t lineStart = 0;

    for (size_t i = 0; i < sourceCode.size(); ++i)
    {
        if (currentLine == line)
        {
            lineStart = i;
            break;
        }
        if (sourceCode[i] == '\n')
        {
            currentLine++;
        }
    }

    if (currentLine != line)
    {
        return "";
    }

    size_t lineEnd = sourceCode.find('\n', lineStart);
    if (lineEnd == std::string_view::npos)
    {
        lineEnd = sourceCode.size();
    }

    size_t prefixLen = std::min(static_cast<size_t>(character), lineEnd - lineStart);
    return std::string(sourceCode.substr(lineStart, prefixLen));
}

/**
 * @brief Skips single-line comment.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this comment.
 */
static bool SkipLineComment(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find('\n', i + 2);
    if (close == std::string_view::npos || close >= end)
    {
        return true;
    }
    i = close + 1;
    return false;
}

/**
 * @brief Skips multi-line block comment.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this comment.
 */
static bool SkipBlockComment(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find("*/", i + 2);
    if (close == std::string_view::npos || close + 2 > end)
    {
        return true;
    }
    i = close + 2;
    return false;
}

/**
 * @brief Skips triple-quoted heredoc string literal.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this string.
 */
static bool SkipHeredoc(std::string_view source, size_t& i, size_t end)
{
    const size_t close = source.find("\"\"\"", i + 3);
    if (close == std::string_view::npos || close + 3 > end)
    {
        return true;
    }
    i = close + 3;
    return false;
}

/**
 * @brief Skips single-line quoted string literal.
 * @param[in] source Source code.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @return True if offset falls inside this string.
 */
static bool SkipQuotedString(std::string_view source, size_t& i, size_t end)
{
    const char c = source[i];
    size_t j = i + 1;
    while (j < source.size() && source[j] != '\n' && source[j] != c)
    {
        if (source[j] == '\\' && j + 1 < source.size() && source[j + 1] != '\n')
        {
            ++j;
        }
        ++j;
    }
    if (end <= j)
    {
        return true;
    }
    i = j + 1;
    return false;
}

/**
 * @brief Scans comment or string literal at current index, updating offset and context.
 * @param[in] source Source code view.
 * @param[in,out] i Scanner index.
 * @param[in] end Maximum scan offset.
 * @param[out] outContext Resulting lexical context if inside a token.
 * @return True if offset falls inside scanned comment or string.
 */
static bool TryScanCommentOrString(std::string_view source, size_t& i, size_t end, LexicalContext& outContext)
{
    const char c = source[i];
    if (c == '/' && i + 1 < source.size())
    {
        if (source[i + 1] == '/')
        {
            outContext = LexicalContext::Comment;
            return SkipLineComment(source, i, end);
        }
        if (source[i + 1] == '*')
        {
            outContext = LexicalContext::Comment;
            return SkipBlockComment(source, i, end);
        }
    }
    if (c == '"' && i + 2 < source.size() && source[i + 1] == '"' && source[i + 2] == '"')
    {
        outContext = LexicalContext::StringLiteral;
        return SkipHeredoc(source, i, end);
    }
    if (c == '"' || c == '\'')
    {
        outContext = LexicalContext::StringLiteral;
        return SkipQuotedString(source, i, end);
    }
    return false;
}

LexicalContext ContextAtOffset(std::string_view source, size_t offset)
{
    const size_t end = std::min(offset, source.size());
    size_t i = 0;

    while (i < end)
    {
        LexicalContext ctx = LexicalContext::Code;
        if (TryScanCommentOrString(source, i, end, ctx))
        {
            return ctx;
        }
        ++i;
    }

    return LexicalContext::Code;
}

bool IsAfterCaseLabelColon(const std::string& prefix)
{
    if (prefix.empty() || prefix.back() != ':')
    {
        return false;
    }
    if (prefix.size() >= 2 && prefix[prefix.size() - 2] == ':')
    {
        return false;
    }

    static const std::regex caseLabelRegex(R"((^|[\s{};])(case\s+[^;]*|default\s*):$)");
    return std::regex_search(prefix, caseLabelRegex);
}

bool IsInsideTemplateArguments(std::string_view prefix)
{
    int depth = 0;
    for (size_t i = prefix.size(); i-- > 0;)
    {
        const char c = prefix[i];
        if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')')
        {
            return false;
        }
        if (c == '>')
        {
            ++depth;
        }
        else if (c == '<')
        {
            if (depth > 0)
            {
                --depth;
                continue;
            }
            if (i == 0)
            {
                return false;
            }
            const char before = prefix[i - 1];
            return std::isalnum(static_cast<unsigned char>(before)) || before == '_';
        }
    }
    return false;
}

bool HasTrailingOpenParen(std::string_view sourceCode, const TargetPosition& position)
{
    if (sourceCode.empty())
    {
        return false;
    }
    size_t offset = utils::LineStartOffset(sourceCode, position.line) + position.character;
    while (offset < sourceCode.size())
    {
        char c = sourceCode[offset];
        if (c == ' ' || c == '\t')
        {
            ++offset;
            continue;
        }
        return c == '(';
    }
    return false;
}

} // namespace angel_lsp::analysis::completion
