#include "analysis/DeclarationRecovery.h"
#include "analysis/SymbolTable.h"
#include <cctype>

namespace angel_lsp::analysis
{
namespace
{

bool SkipComment(std::string_view source, size_t& pos)
{
    const size_t n = source.size();
    if (pos >= n || source[pos] != '/' || pos + 1 >= n)
    {
        return false;
    }

    if (source[pos + 1] == '/')
    {
        const size_t nl = source.find('\n', pos + 2);
        pos = (nl == std::string_view::npos) ? n : (nl + 1);
        return true;
    }
    if (source[pos + 1] == '*')
    {
        const size_t end = source.find("*/", pos + 2);
        pos = (end == std::string_view::npos) ? n : (end + 2);
        return true;
    }
    return false;
}

bool SkipHeredoc(std::string_view source, size_t& pos)
{
    const size_t n = source.size();
    if (pos + 2 < n && source[pos] == '"' && source[pos + 1] == '"' && source[pos + 2] == '"')
    {
        const size_t end = source.find("\"\"\"", pos + 3);
        pos = (end == std::string_view::npos) ? n : (end + 3);
        return true;
    }
    return false;
}

bool SkipQuotedLiteral(std::string_view source, size_t& pos)
{
    const size_t n = source.size();
    if (pos >= n || (source[pos] != '"' && source[pos] != '\''))
    {
        return false;
    }

    const char quote = source[pos++];
    while (pos < n && source[pos] != quote && source[pos] != '\n')
    {
        if (source[pos] == '\\' && pos + 1 < n)
        {
            pos += 2;
            continue;
        }
        ++pos;
    }
    if (pos < n && source[pos] == quote)
    {
        ++pos;
    }
    return true;
}

bool SkipString(std::string_view source, size_t& pos)
{
    return SkipHeredoc(source, pos) || SkipQuotedLiteral(source, pos);
}

bool SkipCommentsAndStrings(std::string_view source, size_t& pos)
{
    return SkipComment(source, pos) || SkipString(source, pos);
}

size_t FindBraceBlockEnd(std::string_view sourceCode, size_t braceStart)
{
    int depth = 1;
    size_t pos = braceStart + 1;
    const size_t n = sourceCode.size();

    while (pos < n && depth > 0)
    {
        if (SkipCommentsAndStrings(sourceCode, pos))
        {
            continue;
        }

        const char c = sourceCode[pos];
        if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            --depth;
            if (depth == 0)
            {
                return pos + 1;
            }
        }
        ++pos;
    }
    return n;
}

size_t SkipModifiers(std::string_view line, size_t pos)
{
    while (pos < line.size())
    {
        const std::string_view rest = line.substr(pos);
        if (rest.starts_with("abstract ") || rest.starts_with("abstract\t"))
            pos += 9;
        else if (rest.starts_with("final ") || rest.starts_with("final\t"))
            pos += 6;
        else if (rest.starts_with("shared ") || rest.starts_with("shared\t"))
            pos += 7;
        else if (rest.starts_with("external ") || rest.starts_with("external\t"))
            pos += 9;
        else if (rest.starts_with("mixin ") || rest.starts_with("mixin\t"))
            pos += 6;
        else
            break;

        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
    }
    return pos;
}

std::string_view MatchKeyword(std::string_view rest, size_t& pos)
{
    if (rest.starts_with("class ") || rest.starts_with("class\t"))
    {
        pos += 6;
        return "class";
    }
    if (rest.starts_with("interface ") || rest.starts_with("interface\t"))
    {
        pos += 10;
        return "interface";
    }
    if (rest.starts_with("enum ") || rest.starts_with("enum\t"))
    {
        pos += 5;
        return "enum";
    }
    if (rest.starts_with("funcdef ") || rest.starts_with("funcdef\t"))
    {
        pos += 8;
        return "funcdef";
    }
    return {};
}

std::string_view ExtractFuncdefName(std::string_view line, size_t pos)
{
    const size_t lparen = line.find('(', pos);
    if (lparen == std::string_view::npos)
        return {};

    size_t end = lparen;
    while (end > pos && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        --end;

    size_t start = end;
    while (start > pos && (isalnum(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '_'))
        --start;

    return (start < end) ? line.substr(start, end - start) : std::string_view{};
}

std::string_view ExtractDeclarationName(std::string_view line, size_t pos, std::string_view kw)
{
    if (kw == "funcdef")
    {
        return ExtractFuncdefName(line, pos);
    }

    const size_t nameStart = pos;
    while (pos < line.size() && (isalnum(static_cast<unsigned char>(line[pos])) || line[pos] == '_'))
    {
        ++pos;
    }

    return line.substr(nameStart, pos - nameStart);
}

bool IsControlKeyword(std::string_view name) noexcept
{
    return name == "if" || name == "for" || name == "while" || name == "switch" || name == "catch" || name == "return";
}

bool TryParseFunctionHeader(std::string_view line, size_t pos, std::string_view& outName) noexcept
{
    const size_t lparen = line.find('(', pos);
    if (lparen == std::string_view::npos || lparen <= pos)
    {
        return false;
    }

    const size_t eq = line.find('=', pos);
    if (eq != std::string_view::npos && eq < lparen)
    {
        return false;
    }

    const size_t quote = line.find_first_of("\"'", pos);
    if (quote != std::string_view::npos && quote < lparen)
    {
        return false;
    }

    std::string_view funcName = ExtractFuncdefName(line, pos);
    if (funcName.empty() || IsControlKeyword(funcName))
    {
        return false;
    }

    const size_t funcStart = line.find(funcName, pos);
    std::string_view beforeFunc =
        (funcStart != std::string_view::npos) ? line.substr(pos, funcStart - pos) : std::string_view{};
    while (!beforeFunc.empty() && (beforeFunc.back() == ' ' || beforeFunc.back() == '\t'))
    {
        beforeFunc.remove_suffix(1);
    }

    if (beforeFunc.empty() || beforeFunc == "return")
    {
        return false;
    }

    outName = funcName;
    return true;
}

bool ParseCandidateHeader(std::string_view line, std::string_view& outKw, std::string_view& outName)
{
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r'))
    {
        ++pos;
    }

    if (pos >= line.size() || line.substr(pos).starts_with("//") || line.substr(pos).starts_with("/*"))
    {
        return false;
    }

    pos = SkipModifiers(line, pos);
    outKw = MatchKeyword(line.substr(pos), pos);
    if (!outKw.empty())
    {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        {
            ++pos;
        }

        outName = ExtractDeclarationName(line, pos, outKw);
        return !outName.empty();
    }

    if (TryParseFunctionHeader(line, pos, outName))
    {
        outKw = "function";
        return true;
    }

    return false;
}

void ComputeLineAndColumn(std::string_view source, size_t targetByte, size_t& outLine, size_t& outCol)
{
    outLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < targetByte && i < source.size(); ++i)
    {
        if (source[i] == '\n')
        {
            ++outLine;
            lineStart = i + 1;
        }
    }
    outCol = targetByte >= lineStart ? (targetByte - lineStart) : 0;
}

} // namespace

size_t FindBalancedDeclarationEnd(std::string_view sourceCode, size_t declStartByte, std::string_view keyword)
{
    const size_t n = sourceCode.size();
    size_t pos = declStartByte;

    if (keyword == "funcdef")
    {
        while (pos < n)
        {
            if (SkipCommentsAndStrings(sourceCode, pos))
                continue;
            if (sourceCode[pos] == ';')
                return pos + 1;
            ++pos;
        }
        return std::string_view::npos;
    }

    while (pos < n)
    {
        if (SkipCommentsAndStrings(sourceCode, pos))
            continue;

        if (sourceCode[pos] == ';')
            return pos + 1;

        if (sourceCode[pos] == '{')
            return FindBraceBlockEnd(sourceCode, pos);

        ++pos;
    }
    return std::string_view::npos;
}

std::vector<RecoveredDeclarationSnippet>
FindSwallowedDeclarations(std::string_view sourceCode, const ankerl::unordered_dense::set<std::string>& knownSymbols)
{
    std::vector<RecoveredDeclarationSnippet> results;
    size_t pos = 0;
    const size_t n = sourceCode.size();

    while (pos < n)
    {
        while (pos < n && (sourceCode[pos] == ' ' || sourceCode[pos] == '\t' || sourceCode[pos] == '\r'))
        {
            ++pos;
        }
        if (pos >= n)
        {
            break;
        }
        if (sourceCode[pos] == '\n')
        {
            ++pos;
            continue;
        }
        if (SkipCommentsAndStrings(sourceCode, pos))
        {
            continue;
        }

        size_t lineEnd = sourceCode.find('\n', pos);
        if (lineEnd == std::string_view::npos)
            lineEnd = n;

        const std::string_view line = sourceCode.substr(pos, lineEnd - pos);
        std::string_view kw;
        std::string_view name;

        if (ParseCandidateHeader(line, kw, name) && !knownSymbols.contains(std::string(name)))
        {
            const size_t declStart = pos;
            const size_t declEnd = FindBalancedDeclarationEnd(sourceCode, declStart, kw);
            if (declEnd != std::string_view::npos && declEnd > declStart)
            {
                size_t startLine = 0;
                size_t startCol = 0;
                ComputeLineAndColumn(sourceCode, declStart, startLine, startCol);

                results.push_back({
                    .snippet = std::string(sourceCode.substr(declStart, declEnd - declStart)),
                    .name = std::string(name),
                    .kind = std::string(kw),
                    .startByte = declStart,
                    .startLine = startLine,
                    .startCol = startCol,
                });

                pos = declEnd;
                continue;
            }
        }

        pos = (lineEnd < n) ? (lineEnd + 1) : n;
    }

    return results;
}

void AdjustRecoveredSymbolOffsets(Symbol& sym, size_t startLine, size_t startCol)
{
    const auto uStartLine = static_cast<uint32_t>(startLine);
    const auto uStartCol = static_cast<uint32_t>(startCol);

    if (sym.startLine == 0)
        sym.startCharacter += uStartCol;
    sym.startLine += uStartLine;

    if (sym.endLine == 0)
        sym.endCharacter += uStartCol;
    sym.endLine += uStartLine;

    if (sym.fullRange.startLine == 0)
        sym.fullRange.startCharacter += uStartCol;
    sym.fullRange.startLine += uStartLine;

    if (sym.fullRange.endLine == 0)
        sym.fullRange.endCharacter += uStartCol;
    sym.fullRange.endLine += uStartLine;

    if (sym.selectionRange.startLine == 0)
        sym.selectionRange.startCharacter += uStartCol;
    sym.selectionRange.startLine += uStartLine;

    if (sym.selectionRange.endLine == 0)
        sym.selectionRange.endCharacter += uStartCol;
    sym.selectionRange.endLine += uStartLine;
}

} // namespace angel_lsp::analysis
