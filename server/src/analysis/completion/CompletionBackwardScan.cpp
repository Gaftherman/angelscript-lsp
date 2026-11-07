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

IndexPeelResult PeelIndexBracketsWithExpr(const std::string& prefix, size_t& s)
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
            std::string inner = prefix.substr(s + 1, closePos - s - 1);
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

size_t SkipInlineWhitespaceBackward(const std::string& text, size_t pos)
{
    while (pos > 0 && (text[pos - 1] == ' ' || text[pos - 1] == '\t'))
    {
        --pos;
    }
    return pos;
}

size_t ExtractIdentifierBackward(const std::string& text, size_t end)
{
    size_t s = end;
    while (s > 0 && (std::isalnum(static_cast<unsigned char>(text[s - 1])) || text[s - 1] == '_'))
    {
        --s;
    }
    return s;
}

std::optional<size_t> FindAccessDotCol(const std::string& prefix)
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
