#include "utils/Utils.h"
#include <string_view>
#include <vector>

#include <lsp/uri.h>

#include <algorithm>
#include <unordered_set>
#include "parser/Primitives.h"

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uriStr)
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

    std::string PathToUri(const std::string &filePath)
    {
        std::string normalized = filePath;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (normalized.empty())
            return "file:///";
        if (normalized[0] == '/')
            return "file://" + normalized;
        return "file:///" + normalized;
    }

    size_t PositionToOffset(const std::string &text, uint32_t line, uint32_t character, PositionEncoding enc)
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

    void ApplyIncrementalChange(std::string &buffer,
                                uint32_t startLine, uint32_t startCharacter,
                                uint32_t endLine, uint32_t endCharacter,
                                const std::string &newText,
                                PositionEncoding enc)
    {
        size_t startOffset = PositionToOffset(buffer, startLine, startCharacter, enc);
        size_t endOffset = PositionToOffset(buffer, endLine, endCharacter, enc);

        if (startOffset <= endOffset)
        {
            buffer.replace(startOffset, endOffset - startOffset, newText);
        }
    }

    bool IsPredefinedFile(const std::string_view &fileUri, const std::string_view extension)
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

    bool IsPrimitiveType(const std::string &typeName)
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
        bool SegmentsMatch(const std::vector<std::string_view> &path, size_t pi,
                           const std::vector<std::string_view> &pattern, size_t qi)
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
    }

    bool MatchesGlob(std::string_view path, std::string_view pattern)
    {
        const std::vector<std::string_view> pathSegments = PathSegments(path);
        const std::vector<std::string_view> patternSegments = PathSegments(pattern);
        return SegmentsMatch(pathSegments, 0, patternSegments, 0);
    }

    bool IsExcludedPath(std::string_view path, const std::vector<std::string> &patterns)
    {
        return std::any_of(patterns.begin(), patterns.end(),
                           [path](const std::string &pattern) { return MatchesGlob(path, pattern); });
    }

    bool IsExcludedDirectory(std::string_view path, const std::vector<std::string> &patterns)
    {
        // A directory is excluded when the directory ITSELF matches, or when a pattern reaches into
        // it. a pattern ending in `/` + `**` names what is inside that directory, not `build`, and pruning is the whole
        // point - filtering the results afterwards still walks every file in it. So the trailing
        // `/**` is dropped before the directory is tested, which is what turns "exclude everything
        // under build" into "do not descend into build".
        return std::any_of(patterns.begin(), patterns.end(), [path](const std::string &pattern)
        {
            std::string_view trimmed(pattern);
            while (trimmed.ends_with("/**") || trimmed.ends_with("\\**"))
            {
                trimmed.remove_suffix(3);
            }
            return !trimmed.empty() && MatchesGlob(path, trimmed);
        });
    }
}
