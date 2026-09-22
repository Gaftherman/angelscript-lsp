#include "utils/Utils.h"
#include <optional>
#include <string_view>
#include <vector>

#include <lsp/uri.h>

#include "parser/Primitives.h"
#include <algorithm>
#include <unordered_set>

namespace angel_lsp::utils
{
std::string UriToPath(const std::string& uriStr)
{
    if (uriStr.rfind("file://", 0) == 0)
    {
        const lsp::Uri uri = lsp::Uri::parse(uriStr);
        if (uri.isValid() && uri.isFileUri())
        {
            return uri.fsPath();
        }
        std::string s = uriStr.substr(7);
#if defined(_WIN32)
        if (!s.empty() && s[0] == '/')
            s = s.substr(1);
#endif
        return s;
    }
#if defined(_WIN32)
    if (!uriStr.empty() && uriStr[0] == '/')
        return uriStr.substr(1);
#endif
    return uriStr;
}

std::string PathToUri(const std::string& filePath)
{
    std::string normalized = filePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty())
        return "file:///";
    if (normalized[0] == '/')
        return "file://" + normalized;
    return "file:///" + normalized;
}

std::string UrlDecode(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            auto fromHex = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            int h1 = fromHex(in[i + 1]);
            int h2 = fromHex(in[i + 2]);
            if (h1 != -1 && h2 != -1)
            {
                out.push_back(static_cast<char>((h1 << 4) | h2));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

size_t PositionToOffset(const std::string& text, uint32_t line, uint32_t character, PositionEncoding enc)
{
    // character is in the negotiated encoding (UTF-16 code units unless the client agreed to
    // UTF-8), while every offset the rest of the server deals in is a byte offset. Resolving it
    // against the line content instead of adding it to the line start is what keeps documents
    // containing non-ASCII text from drifting: the byte offset of a column only equals the
    // column itself on a pure-ASCII line.
    const size_t lineStart = LineStartOffset(text, line);
    if (lineStart >= text.size())
        return text.size();

    const std::string_view lineText = GetLine(text, line);
    return std::min(lineStart + LspCharToByteColumn(lineText, character, enc), text.size());
}

void ApplyIncrementalChange(std::string& buffer, const TextChangeRange& range, const std::string& newText,
                            PositionEncoding enc)
{
    const size_t startOffset = PositionToOffset(buffer, range.startLine, range.startCharacter, enc);
    const size_t endOffset = PositionToOffset(buffer, range.endLine, range.endCharacter, enc);

    if (startOffset <= endOffset)
    {
        buffer.replace(startOffset, endOffset - startOffset, newText);
    }
}

bool IsPredefinedFile(const std::string_view& fileUri, const std::string_view extension)
{
    // AngelScript's own convention is a file named exactly `as.predefined`, and that is what
    // the community's stubs are called. It is not matched by the configured suffix - the
    // default `.as.predefined` wants a dot where such a path has a separator - so it is
    // recognised on its own. Without this the Sven Coop stub, 646 KB describing the entire
    // engine API, is never picked up by a workspace scan and every host type stays invisible.
    constexpr std::string_view k_conventionalName = "as.predefined";
    if (fileUri.ends_with(k_conventionalName))
    {
        const size_t nameStart = fileUri.size() - k_conventionalName.size();
        if (nameStart == 0 || fileUri[nameStart - 1] == '/' || fileUri[nameStart - 1] == '\\')
            return true;
    }

    if (extension.empty())
        return false;
    return fileUri.ends_with(extension) || fileUri.ends_with(fmt::format("/{}", extension));
}

namespace
{
/**
 * @brief Advances pos past all ASCII whitespace characters.
 * @param[in] text Input string.
 * @param[in] pos Starting offset.
 * @return First non-whitespace index or text.size().
 */
size_t SkipWhitespace(std::string_view text, size_t pos)
{
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
    {
        ++pos;
    }
    return pos;
}

/**
 * @brief Skips an optional 'const' qualifier and any surrounding whitespace.
 * @param[in] text Input string.
 * @param[in] pos Starting offset.
 * @return Offset after 'const' and whitespace, or pos if no 'const' found.
 */
size_t SkipConstQualifier(std::string_view text, size_t pos)
{
    pos = SkipWhitespace(text, pos);
    constexpr std::string_view kConst = "const";
    if (pos + kConst.size() <= text.size() && text.substr(pos, kConst.size()) == kConst)
    {
        const size_t after = pos + kConst.size();
        if (after == text.size() || text[after] == ' ' || text[after] == '\t' || text[after] == '\r' ||
            text[after] == '\n' || text[after] == '{')
        {
            return SkipWhitespace(text, after);
        }
    }
    return pos;
}

/**
 * @brief Finds the matching closing brace for an opening brace at openBrace.
 * @param[in] text Input string.
 * @param[in] openBrace Index of opening '{'.
 * @return Index of matching '}', or std::nullopt if unmatched.
 */
std::optional<size_t> FindMatchingBrace(std::string_view text, size_t openBrace)
{
    int depth = 1;
    size_t k = openBrace + 1;
    while (k < text.size() && depth > 0)
    {
        if (text[k] == '{')
        {
            ++depth;
        }
        else if (text[k] == '}')
        {
            --depth;
            if (depth == 0)
            {
                return k;
            }
        }
        ++k;
    }
    return std::nullopt;
}

struct PatternSpan
{
    size_t openBrace{0};
    size_t closeBrace{0};
    size_t nextIndex{0};
};

/**
 * @brief Identifies an inline list pattern like '{repeat T}' following a closing paren.
 * @param[in] text Input string.
 * @param[in] closeParenPos Offset of ')'.
 * @return PatternSpan if an inline pattern followed by ';' is found, otherwise std::nullopt.
 */
std::optional<PatternSpan> FindInlineListPattern(std::string_view text, size_t closeParenPos)
{
    const size_t afterParen = SkipConstQualifier(text, closeParenPos + 1);
    if (afterParen >= text.size() || text[afterParen] != '{')
    {
        return std::nullopt;
    }
    const auto closeBrace = FindMatchingBrace(text, afterParen);
    if (!closeBrace)
    {
        return std::nullopt;
    }
    const size_t afterClose = SkipWhitespace(text, *closeBrace + 1);
    if (afterClose < text.size() && text[afterClose] == ';')
    {
        return PatternSpan{afterParen, *closeBrace, afterClose};
    }
    return std::nullopt;
}

/**
 * @brief Blanks characters in [openBrace, closeBrace] with spaces, preserving newlines.
 * @param[in,out] result String buffer to blank in place.
 * @param[in] openBrace Start offset.
 * @param[in] closeBrace End offset (inclusive).
 */
void BlankPattern(std::string& result, size_t openBrace, size_t closeBrace)
{
    for (size_t b = openBrace; b <= closeBrace; ++b)
    {
        if (result[b] != '\r' && result[b] != '\n')
        {
            result[b] = ' ';
        }
    }
}
} // namespace

std::string SanitizePredefinedContent(std::string_view source)
{
    if (source.empty())
    {
        return {};
    }

    // Quick check: inline list patterns require '{', ')', and ';' to be present.
    if (source.find('{') == std::string_view::npos || source.find(')') == std::string_view::npos ||
        source.find(';') == std::string_view::npos)
    {
        return std::string(source);
    }

    std::string result(source);
    size_t i = 0;
    while (i < result.size())
    {
        if (result[i] == ')')
        {
            if (const auto pattern = FindInlineListPattern(result, i))
            {
                BlankPattern(result, pattern->openBrace, pattern->closeBrace);
                i = pattern->nextIndex;
                continue;
            }
        }
        ++i;
    }

    return result;
}

bool IsPrimitiveType(const std::string& typeName)
{
    return parser::primitives::IsPrimitive(typeName);
}

namespace
{
/** @brief Splits a `/`-separated path into segments, dropping empties. */
std::vector<std::string_view> PathSegments(std::string_view path)
{
    std::vector<std::string_view> segments;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i == path.size() || path[i] == '/' || path[i] == '\\')
        {
            if (i > start)
            {
                segments.push_back(path.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return segments;
}

/** @brief `*` and `?` within one segment. `*` does not cross a separator. */
bool SegmentMatches(std::string_view segment, std::string_view pattern)
{
    size_t s = 0;
    size_t p = 0;
    size_t starAt = std::string_view::npos;
    size_t sAtStar = 0;

    while (s < segment.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == segment[s]))
        {
            ++s;
            ++p;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            starAt = p++;
            sAtStar = s;
        }
        else if (starAt != std::string_view::npos)
        {
            p = starAt + 1;
            s = ++sAtStar;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.size();
}

/** @brief Segment-wise match with `**` spanning any number of segments, including none. */
bool SegmentsMatch(const std::vector<std::string_view>& path, size_t pi, const std::vector<std::string_view>& pattern,
                   size_t qi)
{
    while (qi < pattern.size())
    {
        if (pattern[qi] == "**")
        {
            // `**` at the end matches whatever is left, including nothing.
            if (qi + 1 == pattern.size())
            {
                return true;
            }
            for (size_t skip = pi; skip <= path.size(); ++skip)
            {
                if (SegmentsMatch(path, skip, pattern, qi + 1))
                {
                    return true;
                }
            }
            return false;
        }

        if (pi >= path.size() || !SegmentMatches(path[pi], pattern[qi]))
        {
            return false;
        }
        ++pi;
        ++qi;
    }
    return pi == path.size();
}
} // namespace

bool MatchesGlob(std::string_view path, std::string_view pattern)
{
    const std::vector<std::string_view> pathSegments = PathSegments(path);
    const std::vector<std::string_view> patternSegments = PathSegments(pattern);
    return SegmentsMatch(pathSegments, 0, patternSegments, 0);
}

bool IsExcludedPath(std::string_view path, const std::vector<std::string>& patterns)
{
    return std::any_of(patterns.begin(), patterns.end(),
                       [path](const std::string& pattern) { return MatchesGlob(path, pattern); });
}

bool IsExcludedDirectory(std::string_view path, const std::vector<std::string>& patterns)
{
    // A directory is excluded when the directory ITSELF matches, or when a pattern reaches into
    // it. a pattern ending in `/` + `**` names what is inside that directory, not `build`, and pruning is the whole
    // point - filtering the results afterwards still walks every file in it. So the trailing
    // `/**` is dropped before the directory is tested, which is what turns "exclude everything
    // under build" into "do not descend into build".
    return std::any_of(patterns.begin(), patterns.end(),
                       [path](const std::string& pattern)
                       {
                           std::string_view trimmed(pattern);
                           while (trimmed.ends_with("/**") || trimmed.ends_with("\\**"))
                           {
                               trimmed.remove_suffix(3);
                           }
                           return !trimmed.empty() && MatchesGlob(path, trimmed);
                       });
}

bool IsWithinDirectory(const std::filesystem::path& root, const std::filesystem::path& target)
{
    std::error_code ec;
    std::filesystem::path canonicalRoot = std::filesystem::canonical(root, ec);
    if (ec)
    {
        canonicalRoot = std::filesystem::weakly_canonical(root, ec);
        if (ec)
        {
            return false;
        }
    }

    const std::filesystem::path canonicalTarget = std::filesystem::weakly_canonical(target, ec);
    if (ec)
    {
        return false;
    }

    auto [rootMismatch, targetMismatch] =
        std::mismatch(canonicalRoot.begin(), canonicalRoot.end(), canonicalTarget.begin(), canonicalTarget.end());

    return rootMismatch == canonicalRoot.end();
}
} // namespace angel_lsp::utils
