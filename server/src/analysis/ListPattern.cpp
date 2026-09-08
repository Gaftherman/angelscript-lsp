#include "analysis/ListPattern.h"

#include <cctype>

namespace angel_lsp::analysis
{
    namespace
    {
        /** @brief Recursive-descent cursor over the pattern text. */
        struct Cursor
        {
            std::string_view text;
            size_t pos = 0;
            bool failed = false;

            void SkipSpace()
            {
                while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
                {
                    ++pos;
                }
            }

            bool Peek(char c)
            {
                SkipSpace();
                return pos < text.size() && text[pos] == c;
            }

            bool Take(char c)
            {
                if (!Peek(c))
                {
                    return false;
                }
                ++pos;
                return true;
            }

            /** @brief Consumes `word` when it is next and is not the prefix of a longer name. */
            bool TakeWord(std::string_view word)
            {
                SkipSpace();
                if (text.substr(pos, word.size()) != word)
                {
                    return false;
                }
                const size_t after = pos + word.size();
                if (after < text.size())
                {
                    const char next = text[after];
                    if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
                    {
                        return false;   // `repeatable`, not `repeat`
                    }
                }
                pos = after;
                return true;
            }
        };

        ListPatternNode ParseItem(Cursor &cursor);

        /** @brief `{ item (, item)* }`. */
        ListPatternNode ParseGroup(Cursor &cursor)
        {
            ListPatternNode group;
            group.kind = ListPatternNode::Kind::Group;

            if (!cursor.Take('{'))
            {
                cursor.failed = true;
                return group;
            }

            if (cursor.Take('}'))
            {
                return group;   // `{}` - a group that takes nothing.
            }

            for (;;)
            {
                group.children.push_back(ParseItem(cursor));
                if (cursor.failed)
                {
                    return group;
                }
                if (cursor.Take(','))
                {
                    continue;
                }
                if (cursor.Take('}'))
                {
                    return group;
                }
                cursor.failed = true;
                return group;
            }
        }

        /**
         * @brief `repeat X`, `repeat_same X`, a nested group, or a type name.
         *
         * `repeat_same` differs from `repeat` only in requiring every repetition to have the same
         * length, which is a constraint on sizes rather than on shapes - and shapes are all this
         * pass reads - so it is treated as `repeat` here.
         */
        ListPatternNode ParseItem(Cursor &cursor)
        {
            if (cursor.TakeWord("repeat_same") || cursor.TakeWord("repeat"))
            {
                ListPatternNode repeat;
                repeat.kind = ListPatternNode::Kind::Repeat;
                repeat.children.push_back(ParseItem(cursor));
                return repeat;
            }

            if (cursor.Peek('{'))
            {
                return ParseGroup(cursor);
            }

            cursor.SkipSpace();
            const size_t start = cursor.pos;
            int depth = 0;
            while (cursor.pos < cursor.text.size())
            {
                const char c = cursor.text[cursor.pos];
                if (c == '<')
                {
                    ++depth;
                }
                else if (c == '>')
                {
                    if (depth == 0)
                    {
                        break;
                    }
                    --depth;
                }
                else if (depth == 0 && (c == ',' || c == '}' || c == '{'))
                {
                    break;
                }
                ++cursor.pos;
            }

            ListPatternNode type;
            type.kind = ListPatternNode::Kind::Type;
            type.typeName = std::string(cursor.text.substr(start, cursor.pos - start));

            // Trailing space before the delimiter, and the `&in` / `const` a registration string
            // may carry, are not part of the type's identity here.
            while (!type.typeName.empty() &&
                   std::isspace(static_cast<unsigned char>(type.typeName.back())))
            {
                type.typeName.pop_back();
            }

            if (type.typeName.empty())
            {
                cursor.failed = true;
            }
            return type;
        }
    }

    ListPattern ParseListPattern(std::string_view text)
    {
        ListPattern pattern;

        Cursor cursor{ text };
        cursor.SkipSpace();
        if (!cursor.Peek('{'))
        {
            return pattern;
        }

        pattern.root = ParseGroup(cursor);
        if (cursor.failed)
        {
            return pattern;
        }

        cursor.SkipSpace();
        if (cursor.pos != cursor.text.size())
        {
            return pattern;   // Trailing junk: not a pattern this server understands.
        }

        pattern.valid = true;
        return pattern;
    }

    std::string FindListPatternTag(const std::string &sourceCode, uint32_t declStartLine)
    {
        // Walks up from the declaration through its comment block. Deliberately not routed through
        // ExtractDocComment, which renders markdown for hover and would have to be reverse-engineered
        // to get the tag's text back out intact.
        std::vector<std::string_view> lines;
        size_t lineStart = 0;
        for (size_t i = 0; i <= sourceCode.size(); ++i)
        {
            if (i == sourceCode.size() || sourceCode[i] == '\n')
            {
                size_t end = i;
                if (end > lineStart && sourceCode[end - 1] == '\r')
                {
                    --end;
                }
                lines.emplace_back(sourceCode.data() + lineStart, end - lineStart);
                lineStart = i + 1;
            }
        }

        if (declStartLine >= lines.size())
        {
            return {};
        }

        constexpr std::string_view k_tag = "@listpattern";

        // Inside the declaration first, then above it. The inline form lands on a member line of
        // the class - RewriteInlineListPatterns put it there - and the doc-comment form sits on the
        // lines before. A class carries one list factory, so the first hit either way is the
        // answer; searching the body first means an inline pattern wins over a stale tag above.
        for (size_t i = declStartLine; i < lines.size(); ++i)
        {
            std::string_view line = lines[i];

            // Stop at the line that closes the declaration, so one class cannot read the next
            // one's pattern. Column zero, which is where this file format closes a class.
            if (i > declStartLine && !line.empty() && line.front() == '}')
            {
                break;
            }

            const size_t tagPos = line.find(k_tag);
            if (tagPos == std::string_view::npos)
            {
                continue;
            }

            std::string_view rest = line.substr(tagPos + k_tag.size());
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            {
                rest.remove_prefix(1);
            }
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
            {
                rest.remove_suffix(1);
            }
            if (!rest.empty())
            {
                return std::string(rest);
            }
        }

        for (size_t i = declStartLine; i-- > 0;)
        {
            std::string_view line = lines[i];
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            {
                line.remove_prefix(1);
            }
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            {
                line.remove_suffix(1);
            }

            if (line.empty())
            {
                continue;   // A blank line inside a comment block does not end it.
            }

            const bool isComment = line.starts_with("//") || line.starts_with("*") ||
                                   line.starts_with("/*") || line.ends_with("*/");
            if (!isComment)
            {
                break;   // Reached real code above the declaration.
            }

            const size_t tagPos = line.find(k_tag);
            if (tagPos == std::string_view::npos)
            {
                continue;
            }

            std::string_view rest = line.substr(tagPos + k_tag.size());
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            {
                rest.remove_prefix(1);
            }
            // A block comment's closing delimiter is not part of the pattern.
            if (rest.ends_with("*/"))
            {
                rest.remove_suffix(2);
            }
            while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
            {
                rest.remove_suffix(1);
            }

            return std::string(rest);
        }

        return {};
    }

    std::string RewriteInlineListPatterns(const std::string &source)
    {
        // `) {` is the whole signal: a brace that follows a parameter list. In a declaration file
        // nothing else puts one there - a function with a body would be a definition, and a stub
        // has none. Cheap to test for, and it is what keeps this from scanning class bodies.
        if (source.find(") {") == std::string::npos && source.find("){") == std::string::npos)
        {
            return source;
        }

        std::string out;
        out.reserve(source.size() + 64);

        size_t lineStart = 0;
        while (lineStart <= source.size())
        {
            size_t lineEnd = source.find('\n', lineStart);
            const bool last = lineEnd == std::string::npos;
            if (last)
            {
                lineEnd = source.size();
            }

            const std::string_view line(source.data() + lineStart, lineEnd - lineStart);

            // A line that already carries a `//@listpattern` marker was rewritten on a previous
            // pass. Skip it as insurance against double-application, which would otherwise match
            // the pattern inside the appended comment and rewrite it again.
            if (line.find("//@listpattern") != std::string_view::npos)
            {
                out.append(line);
            }
            else
            {
                // The pattern sits between the `)` that closes the parameters and the `;` that ends
                // the declaration. Both must be on this line, in that order, with a `{...}` between.
                const size_t closeParen = line.rfind(')');
                const size_t open = closeParen == std::string_view::npos
                                        ? std::string_view::npos
                                        : line.find('{', closeParen);
                const size_t close = open == std::string_view::npos
                                         ? std::string_view::npos
                                         : line.rfind('}');
                const size_t semi = close == std::string_view::npos
                                        ? std::string_view::npos
                                        : line.find(';', close);

                const bool hasPattern = open != std::string_view::npos && close > open &&
                                        semi != std::string_view::npos;

                if (!hasPattern)
                {
                    out.append(line);
                }
                else
                {
                    const std::string_view pattern = line.substr(open, close - open + 1);

                    // Blanked in place, then re-stated at the end of the line. Blanked rather than
                    // deleted on purpose: a stub is a file the user navigates, and closing the gap
                    // would pull the `;` left and send go-to-definition to the wrong column here.
                    out.append(line.substr(0, open));
                    out.append(pattern.size(), ' ');
                    out.append(line.substr(close + 1));
                    out.append("//@listpattern ");
                    out.append(pattern);
                }
            }

            if (last)
            {
                break;
            }
            out.push_back('\n');
            lineStart = lineEnd + 1;
        }

        return out;
    }
}
