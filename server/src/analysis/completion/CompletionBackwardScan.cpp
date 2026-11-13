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

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace angel_lsp::analysis::completion
{

std::string GetBackwardDeclarationText(std::string_view sourceCode, uint32_t line, uint32_t character)
{
    uint32_t curLine = 0;
    size_t offset = 0;
    for (size_t i = 0; i < sourceCode.size() && curLine < line; ++i)
    {
        if (sourceCode[i] == '\n')
        {
            ++curLine;
            offset = i + 1;
        }
    }
    offset += character;
    if (offset > sourceCode.size())
    {
        offset = sourceCode.size();
    }

    size_t start = offset;
    size_t count = 0;
    while (start > 0 && count < 500)
    {
        const char ch = sourceCode[start - 1];
        if (ch == '{' || ch == '}' || ch == ';')
        {
            break;
        }
        --start;
        ++count;
    }
    return std::string(sourceCode.substr(start, offset - start));
}

IndexPeelResult PeelIndexBracketsWithExpr(std::string_view prefix, size_t& s)
{
    IndexPeelResult res;
    while (s > 0 && prefix[s - 1] == ']')
    {
        size_t closePos = s - 1;
        --s;
        int depth = 1;
        while (s > 0 && depth > 0)
        {
            if (prefix[s - 1] == ']')
            {
                ++depth;
            }
            else if (prefix[s - 1] == '[')
            {
                --depth;
            }
            --s;
        }
        if (res.lastIndexExpr.empty() && closePos > s)
        {
            const std::string_view inner = prefix.substr(s + 1, closePos - s - 1);
            size_t start = inner.find_first_not_of(" \t");
            size_t end = inner.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
            {
                res.lastIndexExpr = inner.substr(start, end - start + 1);
            }
        }
        ++res.count;
        while (s > 0 && (prefix[s - 1] == ' ' || prefix[s - 1] == '\t'))
        {
            --s;
        }
    }
    return res;
}

bool PeelCallParentheses(std::string_view prefix, size_t& s)
{
    size_t cur = s;
    if (cur > 0 && prefix[cur - 1] == ')')
    {
        --cur;
        int depth = 1;
        while (cur > 0 && depth > 0)
        {
            if (prefix[cur - 1] == ')')
            {
                ++depth;
            }
            else if (prefix[cur - 1] == '(')
            {
                --depth;
            }
            --cur;
        }
        if (depth == 0)
        {
            s = cur;
            while (s > 0 && (prefix[s - 1] == ' ' || prefix[s - 1] == '\t'))
            {
                --s;
            }
            return true;
        }
    }
    return false;
}

size_t SkipInlineWhitespaceBackward(std::string_view text, size_t pos)
{
    while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t'))
    {
        --pos;
    }
    return pos;
}

size_t SkipWhitespaceAndCommentsBackward(std::string_view text, size_t pos)
{
    while (pos > 0)
    {
        if (text[pos - 1] == ' ' || text[pos - 1] == '\t' || text[pos - 1] == '\r' || text[pos - 1] == '\n')
        {
            --pos;
            continue;
        }
        if (pos >= 2 && text[pos - 1] == '/' && text[pos - 2] == '*')
        {
            pos -= 2;
            while (pos >= 2 && !(text[pos - 1] == '*' && text[pos - 2] == '/'))
            {
                --pos;
            }
            if (pos >= 2)
            {
                pos -= 2;
            }
            continue;
        }
        if (pos >= 2 && text[pos - 1] == '/' && text[pos - 2] == '/')
        {
            pos -= 2;
            while (pos > 0 && text[pos - 1] != '\n')
            {
                --pos;
            }
            continue;
        }
        break;
    }
    return pos;
}

/**
 * @brief Scans backward from a given offset to locate the starting byte position of an alphanumeric or underscore
 * identifier token.
 * @param[in] text Source text buffer to inspect backwards.
 * @param[in] end Exclusive byte offset where backward scanning begins (must be less than or equal to text.size()).
 * @return Starting byte offset of the identifier within text, or end if the preceding character is not an alphanumeric
 * or underscore.
 */
size_t ExtractIdentifierBackward(std::string_view text, size_t end)
{
    size_t s = end;
    while (s > 0 && (std::isalnum(static_cast<unsigned char>(text[s - 1])) || text[s - 1] == '_'))
    {
        --s;
    }
    return s;
}

std::optional<size_t> FindAccessDotCol(std::string_view prefix)
{
    size_t i = prefix.size();
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(prefix[i - 1])) || prefix[i - 1] == '_'))
    {
        --i;
    }
    while (i > 0 && (prefix[i - 1] == ' ' || prefix[i - 1] == '\t'))
    {
        --i;
    }
    if (i == 0 || prefix[i - 1] != '.' || IsNumericLiteralDot(prefix, i - 1))
    {
        return std::nullopt;
    }
    return i - 1;
}

/**
 * @brief Extracts the trailing uncommitted identifier segment from a source line prefix to serve as a filtering query
 * for completion candidates.
 * @param[in] linePrefix Source text on the current line ending at the completion cursor position.
 * @return Substring view of trailing alphanumeric and underscore characters, or an empty view if the cursor immediately
 * follows whitespace or punctuation.
 * @note The returned string_view borrows the underlying buffer of linePrefix and remains valid only for that buffer's
 * lifetime.
 */
std::string_view ExtractQueryPrefix(std::string_view linePrefix)
{
    size_t i = linePrefix.size();
    while (i > 0 && (std::isalnum(static_cast<unsigned char>(linePrefix[i - 1])) || linePrefix[i - 1] == '_'))
    {
        --i;
    }
    return linePrefix.substr(i);
}

} // namespace angel_lsp::analysis::completion
