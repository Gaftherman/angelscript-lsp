#include "analysis/diagnostics/DiagnosticSuppression.h"

#include "analysis/index/NodeIndex.h"
#include "utils/Utils.h"

#include <cctype>
#include <unordered_map>

namespace angel_lsp::analysis
{

static std::string_view StripCommentDelimiters(std::string_view text) noexcept
{
    if (text.starts_with("//"))
        text.remove_prefix(2);
    else if (text.starts_with("/*"))
        text.remove_prefix(2);

    if (text.ends_with("*/"))
        text.remove_suffix(2);

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

static std::vector<std::string_view> TokenizeDirective(std::string_view text)
{
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() &&
               (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',' || text[i] == ';'))
            ++i;
        if (i >= text.size())
            break;
        size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ',' &&
               text[i] != ';')
            ++i;
        const auto token = text.substr(start, i - start);
        if (token.starts_with(k_diagnosticExplanationDelimiter))
        {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

void DiagnosticSuppressionMap::AddSuppression(std::string_view code, uint32_t startLine, uint32_t endLine)
{
    m_suppressions.push_back({NormalizeDiagnosticSelector(code), startLine, endLine});
}

bool DiagnosticSuppressionMap::IsSuppressed(std::string_view code, uint32_t line) const
{
    const auto canonical = GetCanonicalDiagnosticCode(code);
    const auto group = GetDiagnosticGroup(canonical);
    const auto category = GetDiagnosticCategory(canonical);
    for (const auto& s : m_suppressions)
    {
        if (line >= s.startLine && line <= s.endLine)
        {
            if (s.code == "all" || s.code == canonical || s.code == group || s.code == category)
                return true;
        }
    }
    return false;
}

static void ProcessCommentDirective(std::string_view rawText, uint32_t line, DiagnosticSuppressionMap& map,
                                    std::unordered_map<std::string_view, std::vector<uint32_t>>& openRanges)
{
    std::string_view content = StripCommentDelimiters(rawText);
    auto tokens = TokenizeDirective(content);
    if (tokens.empty())
        return;

    std::string_view action = tokens[0];
    const bool isDisableLine = utils::CaseInsensitiveEquals(action, "disable-line");
    const bool isDisableNextLine = utils::CaseInsensitiveEquals(action, "disable-next-line");
    const bool isDisable = utils::CaseInsensitiveEquals(action, "disable");
    const bool isEnable = utils::CaseInsensitiveEquals(action, "enable");

    if (!isDisableLine && !isDisableNextLine && !isDisable && !isEnable)
        return;

    for (size_t k = 1; k < tokens.size(); ++k)
    {
        const std::string_view codeKey = NormalizeDiagnosticSelector(tokens[k]);
        if (isDisableLine)
        {
            map.AddSuppression(codeKey, line, line);
        }
        else if (isDisableNextLine)
        {
            map.AddSuppression(codeKey, line + 1, line + 1);
        }
        else if (isDisable)
        {
            openRanges[codeKey].push_back(line);
        }
        else if (isEnable)
        {
            auto it = openRanges.find(codeKey);
            if (it != openRanges.end() && !it->second.empty())
            {
                uint32_t start = it->second.back();
                it->second.pop_back();
                map.AddSuppression(codeKey, start, line > 0 ? line - 1 : 0);
            }
        }
    }
}

static void ScanFallbackComments(std::string_view sourceCode, DiagnosticSuppressionMap& map,
                                 std::unordered_map<std::string_view, std::vector<uint32_t>>& openRanges)
{
    uint32_t line = 0;
    size_t pos = 0;
    while (pos < sourceCode.size())
    {
        size_t nextPos = sourceCode.find('\n', pos);
        if (nextPos == std::string_view::npos)
            nextPos = sourceCode.size();
        std::string_view lineStr = sourceCode.substr(pos, nextPos - pos);
        size_t commentIdx = lineStr.find("//");
        if (commentIdx != std::string_view::npos)
            ProcessCommentDirective(lineStr.substr(commentIdx), line, map, openRanges);
        pos = nextPos + 1;
        ++line;
    }
}

DiagnosticSuppressionMap ParseDiagnosticSuppressions(std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    DiagnosticSuppressionMap map;
    std::unordered_map<std::string_view, std::vector<uint32_t>> openRanges;

    if (nodeIndex)
    {
        for (const NodeIndex::CommentRange& comment : nodeIndex->CommentRanges())
        {
            if (comment.startByte < sourceCode.size() && comment.endByte <= sourceCode.size() &&
                comment.startByte <= comment.endByte)
            {
                const std::string_view commentText =
                    sourceCode.substr(comment.startByte, comment.endByte - comment.startByte);
                ProcessCommentDirective(commentText, comment.startLine, map, openRanges);
            }
        }
    }
    else
    {
        ScanFallbackComments(sourceCode, map, openRanges);
    }

    for (const auto& [codeKey, starts] : openRanges)
    {
        for (uint32_t start : starts)
            map.AddSuppression(codeKey, start, UINT32_MAX);
    }

    return map;
}

} // namespace angel_lsp::analysis
