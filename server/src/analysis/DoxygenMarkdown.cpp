#include "analysis/DoxygenMarkdown.h"
#include "parser/DoxygenParser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{
    namespace
    {
        /** @brief Strips leading and trailing ASCII whitespace. */
        std::string Trim(const std::string &str)
        {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, last - first + 1);
        }

        /** @brief Strips leading ASCII whitespace. */
        std::string TrimLeading(const std::string &str)
        {
            size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            return str.substr(first);
        }

        /** @brief Strips trailing ASCII whitespace in place. */
        void TrimTrailing(std::string &str)
        {
            size_t last = str.find_last_not_of(" \t\r\n");
            if (last == std::string::npos)
            {
                str.clear();
            }
            else
            {
                str.erase(last + 1);
            }
        }

        /** @brief Converts ASCII string to lowercase. */
        std::string ToLower(const std::string &str)
        {
            std::string out = str;
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        /** @brief Splits text into lines, stripping trailing carriage returns. */
        std::vector<std::string> SplitLines(const std::string &str)
        {
            std::vector<std::string> lines;
            std::string line;
            std::istringstream stream(str);
            while (std::getline(stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(std::move(line));
            }
            return lines;
        }

        /**
         * @brief Strips leading comment star decoration [ \t]*\*[ \t]? from a line.
         *
         * Required for lines after the first in multi-line descriptions and code blocks.
         */
        std::string StripLineLeadingStar(const std::string &line)
        {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            {
                ++i;
            }
            if (i < line.size() && line[i] == '*')
            {
                ++i;
                if (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
                {
                    ++i;
                }
            }
            return line.substr(i);
        }

        /**
         * @brief Cleans description lines by stripping line decorations and joining with single spaces.
         *
         * Strips leading [ \t]*\*[ \t]? from every line after the first, trims trailing whitespace,
         * and joins non-empty lines with a single space.
         */
        std::string CleanDescriptionLines(const std::string &raw)
        {
            auto lines = SplitLines(raw);
            if (lines.empty())
            {
                return "";
            }

            std::vector<std::string> cleaned;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string l = lines[i];
                if (i > 0)
                {
                    l = StripLineLeadingStar(l);
                }
                l = Trim(l);
                if (!l.empty())
                {
                    cleaned.push_back(std::move(l));
                }
            }

            std::string result;
            for (size_t i = 0; i < cleaned.size(); ++i)
            {
                if (i > 0)
                {
                    result += " ";
                }
                result += cleaned[i];
            }
            return Trim(result);
        }

        /**
         * @brief Rewrites at-form inline formatting commands (@c, @p, @b, @a, @e, @em) in verbatim text.
         *
         * The tree-sitter-doxygen grammar's _text token (/[^*{}@\\\s][^*!{}\\\n]*.../) swallows '@'
         * characters after the first character of text into anonymous text, so at-form inline commands
         * are never emitted as tag nodes. We rewrite them directly in raw text runs:
         *   @c X, @p X       -> `X`
         *   @b X             -> **X**
         *   @a X, @e X, @em X -> *X*
         * where X is the next whitespace-delimited word, only when preceded by whitespace or start-of-run.
         */
        std::string RewriteAtInlineCommands(const std::string &text)
        {
            if (text.empty())
            {
                return "";
            }

            std::string result;
            result.reserve(text.size());
            size_t i = 0;
            while (i < text.size())
            {
                if (text[i] == '@' || text[i] == '\\')
                {
                    bool atStartOrWs = (i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) ||
                                        text[i - 1] == '(' || text[i - 1] == '[');
                    if (atStartOrWs)
                    {
                        size_t cmdLen = 0;
                        std::string_view cmd;
                        if (i + 3 <= text.size() && text[i + 1] == 'e' && text[i + 2] == 'm')
                        {
                            cmdLen = 2;
                            cmd = "em";
                        }
                        else if (i + 2 <= text.size())
                        {
                            char c = text[i + 1];
                            if (c == 'c' || c == 'p' || c == 'b' || c == 'a' || c == 'e')
                            {
                                cmdLen = 1;
                                cmd = std::string_view(&text[i + 1], 1);
                            }
                        }

                        if (cmdLen > 0 && i + 1 + cmdLen < text.size() &&
                            std::isspace(static_cast<unsigned char>(text[i + 1 + cmdLen])))
                        {
                            size_t argStart = i + 1 + cmdLen;
                            while (argStart < text.size() && (text[argStart] == ' ' || text[argStart] == '\t'))
                            {
                                ++argStart;
                            }

                            if (argStart < text.size() && !std::isspace(static_cast<unsigned char>(text[argStart])))
                            {
                                size_t argEnd = argStart;
                                while (argEnd < text.size() && !std::isspace(static_cast<unsigned char>(text[argEnd])))
                                {
                                    ++argEnd;
                                }

                                std::string X = text.substr(argStart, argEnd - argStart);
                                if (cmd == "c" || cmd == "p")
                                {
                                    result += "`" + X + "`";
                                }
                                else if (cmd == "b")
                                {
                                    result += "**" + X + "**";
                                }
                                else // "a", "e", "em"
                                {
                                    result += "*" + X + "*";
                                }

                                i = argEnd;
                                continue;
                            }
                        }
                    }
                }

                result += text[i];
                ++i;
            }

            return result;
        }

        /**
         * @brief Extracts the content lines of a raw comment, whichever way it was written.
         *
         * Strips the delimiters - `/\*`, `*\/`, `///`, `//`, `//!` - and the per-line star
         * decoration, while preserving the line breaks and the indentation a code block depends on.
         */
        std::vector<std::string> ExtractCommentLines(const std::string &rawComment)
        {
            std::string trimmed = Trim(rawComment);
            if (trimmed.empty())
            {
                return {};
            }

            auto rawLines = SplitLines(rawComment);
            std::vector<std::string> contents;

            if (trimmed.starts_with("/*"))
            {
                bool inBlock = false;
                for (const auto &line : rawLines)
                {
                    if (!inBlock)
                    {
                        size_t openPos = line.find("/*");
                        if (openPos == std::string::npos)
                        {
                            continue;
                        }
                        inBlock = true;
                        size_t start = openPos + 2;
                        size_t closePos = line.rfind("*/");
                        if (closePos != std::string::npos && closePos >= start)
                        {
                            while (start < closePos && (line[start] == '*' || line[start] == '!'))
                            {
                                ++start;
                            }
                            // Single-line block comment: /** ... */
                            std::string inner = line.substr(start, closePos - start);
                            inner = Trim(inner);
                            if (!inner.empty())
                            {
                                contents.push_back(std::move(inner));
                            }
                            inBlock = false;
                            break;
                        }

                        while (start < line.size() && (line[start] == '*' || line[start] == '!'))
                        {
                            ++start;
                        }

                        std::string afterOpen = line.substr(start);
                        if (!Trim(afterOpen).empty())
                        {
                            if (!afterOpen.empty() && afterOpen.front() == ' ')
                            {
                                afterOpen = afterOpen.substr(1);
                            }
                            contents.push_back(std::move(afterOpen));
                        }
                        continue;
                    }

                    // Inside multi-line block comment
                    size_t closePos = line.find("*/");
                    if (closePos != std::string::npos)
                    {
                        std::string beforeClose = line.substr(0, closePos);
                        beforeClose = StripLineLeadingStar(beforeClose);
                        TrimTrailing(beforeClose);
                        if (!beforeClose.empty())
                        {
                            contents.push_back(std::move(beforeClose));
                        }
                        inBlock = false;
                        break;
                    }

                    std::string stripped = StripLineLeadingStar(line);
                    contents.push_back(std::move(stripped));
                }
            }
            else
            {
                for (const auto &line : rawLines)
                {
                    size_t pos = line.find("//");
                    if (pos == std::string::npos)
                    {
                        continue;
                    }
                    std::string sub = line.substr(pos + 2);
                    while (sub.starts_with('/') || sub.starts_with('!'))
                    {
                        sub = sub.substr(1);
                    }
                    if (!sub.empty() && sub.front() == ' ')
                    {
                        sub = sub.substr(1);
                    }
                    contents.push_back(std::move(sub));
                }
            }

            return contents;
        }

        enum class CommentSegmentKind
        {
            Tag,
            Text,
            VerbatimCode
        };

        struct CommentSegment
        {
            CommentSegmentKind kind = CommentSegmentKind::Text;
            std::vector<std::string> lines;
            std::string language;
        };

        /**
         * @brief Checks if a line begins with a Doxygen block command (@name or \name).
         *
         * Lines starting with inline formatting commands (\b, \e, \em, \a, \c, \p and their @-forms)
         * are NOT block commands and must not start a new segment; they belong to the block above them.
         */
        bool IsBlockCommand(std::string_view line, size_t &cmdEnd)
        {
            if (line.size() < 2 || (line[0] != '@' && line[0] != '\\'))
            {
                return false;
            }
            if (!std::isalpha(static_cast<unsigned char>(line[1])))
            {
                return false;
            }

            size_t i = 1;
            while (i < line.size() && std::isalpha(static_cast<unsigned char>(line[i])))
            {
                ++i;
            }

            std::string cmdName = ToLower(std::string(line.substr(1, i - 1)));

            // Inline formatting commands must NOT start a segment:
            if (cmdName == "b" || cmdName == "e" || cmdName == "em" ||
                cmdName == "a" || cmdName == "c" || cmdName == "p")
            {
                return false;
            }

            // Verbatim code commands are handled separately:
            if (cmdName == "code" || cmdName == "endcode")
            {
                return false;
            }

            cmdEnd = i;
            return true;
        }

        /**
         * @brief Extracts the brief description in the pre-pass and removes it from lines.
         *
         * The brief is decided entirely by the pre-pass and is REMOVED from the lines before segmentation.
         * Never letting the grammar see the brief prevents brief_description from swallowing the subsequent
         * tag and avoids runaway implicit-brief regex matching across tags.
         */
        std::string ExtractBrief(std::vector<std::string> &lines)
        {
            // 1. Check for explicit brief (@brief, \brief, @short, \short)
            size_t explicitBriefIdx = std::string::npos;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                std::string trimmed = TrimLeading(lines[i]);
                if (trimmed.starts_with("@brief") || trimmed.starts_with("\\brief") ||
                    trimmed.starts_with("@short") || trimmed.starts_with("\\short"))
                {
                    size_t cmdLen = 6;
                    if (trimmed.size() == cmdLen || std::isspace(static_cast<unsigned char>(trimmed[cmdLen])))
                    {
                        explicitBriefIdx = i;
                        break;
                    }
                }
            }

            if (explicitBriefIdx != std::string::npos)
            {
                std::string trimmed = TrimLeading(lines[explicitBriefIdx]);
                size_t cmdLen = 6;
                std::string briefText = Trim(trimmed.substr(cmdLen));
                lines.erase(lines.begin() + explicitBriefIdx);

                // If brief was on its own line (empty) or did not end with sentence-ending punctuation,
                // absorb following non-empty continuation text (until blank line, code, or block command).
                while (explicitBriefIdx < lines.size())
                {
                    std::string nextTrimmed = TrimLeading(lines[explicitBriefIdx]);
                    if (nextTrimmed.empty())
                    {
                        break;
                    }
                    if (nextTrimmed.starts_with("@code") || nextTrimmed.starts_with("\\code"))
                    {
                        break;
                    }
                    size_t cmdEnd = 0;
                    if (IsBlockCommand(nextTrimmed, cmdEnd))
                    {
                        break;
                    }
                    if (briefText.empty() ||
                        (!briefText.ends_with('.') && !briefText.ends_with('!') && !briefText.ends_with('?')))
                    {
                        if (!briefText.empty())
                        {
                            briefText += " ";
                        }
                        briefText += Trim(lines[explicitBriefIdx]);
                        lines.erase(lines.begin() + explicitBriefIdx);
                    }
                    else
                    {
                        break;
                    }
                }

                briefText = RewriteAtInlineCommands(briefText);
                return CleanDescriptionLines(briefText);
            }

            // 2. Implicit brief: first non-empty content line if not a block command or code
            size_t firstContentIdx = std::string::npos;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (!Trim(lines[i]).empty())
                {
                    firstContentIdx = i;
                    break;
                }
            }

            if (firstContentIdx == std::string::npos)
            {
                return "";
            }

            std::string firstLine = TrimLeading(lines[firstContentIdx]);
            if (firstLine.starts_with("@code") || firstLine.starts_with("\\code"))
            {
                return "";
            }
            size_t cmdEnd = 0;
            if (IsBlockCommand(firstLine, cmdEnd))
            {
                return "";
            }

            std::string briefText;
            size_t dotPos = firstLine.find('.');
            if (dotPos == std::string::npos)
            {
                briefText = Trim(firstLine);
                lines.erase(lines.begin() + firstContentIdx);
            }
            else
            {
                briefText = Trim(firstLine.substr(0, dotPos + 1));
                std::string remainder = TrimLeading(firstLine.substr(dotPos + 1));
                if (remainder.empty())
                {
                    lines.erase(lines.begin() + firstContentIdx);
                }
                else
                {
                    lines[firstContentIdx] = remainder;
                }
            }

            briefText = RewriteAtInlineCommands(briefText);
            return CleanDescriptionLines(briefText);
        }

        /**
         * @brief Segments comment lines into Tag, Text, and VerbatimCode blocks.
         *
         * WHY PRE-PASS SEGMENTATION IS REQUIRED:
         * In tree-sitter-doxygen, brief_description repeats tag_name:
         *   brief_description: prec.right(repeat1(choice($.brief_text, $.tag_name)))
         * so a whole comment handed to this grammar in one piece loses the tag after its brief,
         * and a blank line does not stop it. Without this pre-pass segmentation, the grammar
         * absorbs any tag immediately following a brief into brief_description rather than
         * forming a distinct tag node.
         *
         * To fix this:
         * 1. The brief is decided entirely in the pre-pass and removed from the lines so the
         *    grammar never sees it.
         * 2. The segmenter cuts lines into TAG segments (each starting with a block command and
         *    continuing over multi-line descriptions), TEXT segments (body paragraphs broken
         *    on blank lines), and VERBATIM segments (@code ... @endcode).
         * 3. Lines starting with inline commands (\b, \e, \em, \a, \c, \p and their @-forms) are
         *    NOT block commands and must not start a segment; they belong to the block above them.
         * 4. Each TAG and TEXT segment is wrapped in its own synthetic comment and parsed on its own.
         */
        std::vector<CommentSegment> SegmentCommentLines(const std::vector<std::string> &lines)
        {
            std::vector<CommentSegment> segments;
            CommentSegment currentSeg;
            bool hasActiveSeg = false;

            bool inCode = false;
            CommentSegment currentCode;
            currentCode.kind = CommentSegmentKind::VerbatimCode;

            auto flushActiveSeg = [&]()
            {
                if (hasActiveSeg && !currentSeg.lines.empty())
                {
                    segments.push_back(std::move(currentSeg));
                }
                currentSeg = CommentSegment{};
                hasActiveSeg = false;
            };

            for (const auto &line : lines)
            {
                if (inCode)
                {
                    std::string trimmed = Trim(line);
                    if (trimmed.starts_with("@endcode") || trimmed.starts_with("\\endcode"))
                    {
                        inCode = false;
                        segments.push_back(std::move(currentCode));
                        currentCode = CommentSegment{};
                        currentCode.kind = CommentSegmentKind::VerbatimCode;
                        continue;
                    }
                    currentCode.lines.push_back(line);
                    continue;
                }

                std::string trimmedLeading = TrimLeading(line);

                // Verbatim code start: @code / \code
                if (trimmedLeading.starts_with("@code") || trimmedLeading.starts_with("\\code"))
                {
                    size_t cmdLen = 5;
                    std::string afterCmd = trimmedLeading.substr(cmdLen);
                    if (afterCmd.empty() || std::isspace(static_cast<unsigned char>(afterCmd.front())) ||
                        afterCmd.front() == '{' || afterCmd.front() == '.')
                    {
                        flushActiveSeg();
                        inCode = true;
                        currentCode = CommentSegment{};
                        currentCode.kind = CommentSegmentKind::VerbatimCode;

                        std::string lang = Trim(afterCmd);
                        if (lang.starts_with('{') && lang.ends_with('}'))
                        {
                            lang = lang.substr(1, lang.size() - 2);
                        }
                        lang = Trim(lang);
                        if (lang.starts_with('.'))
                        {
                            lang = lang.substr(1);
                        }
                        currentCode.language = Trim(lang);
                        continue;
                    }
                }

                // Blank line breaks text paragraphs and terminates active blocks
                if (Trim(line).empty())
                {
                    flushActiveSeg();
                    continue;
                }

                // Check for block command (@name or \name)
                size_t cmdEnd = 0;
                if (IsBlockCommand(trimmedLeading, cmdEnd))
                {
                    flushActiveSeg();
                    currentSeg.kind = CommentSegmentKind::Tag;
                    hasActiveSeg = true;

                    // Normalize leading \name to @name at start of segment line
                    std::string normLine = line;
                    size_t firstNonSpace = 0;
                    while (firstNonSpace < normLine.size() &&
                           (normLine[firstNonSpace] == ' ' || normLine[firstNonSpace] == '\t'))
                    {
                        ++firstNonSpace;
                    }
                    if (firstNonSpace < normLine.size() && normLine[firstNonSpace] == '\\')
                    {
                        normLine[firstNonSpace] = '@';
                    }
                    currentSeg.lines.push_back(std::move(normLine));
                    continue;
                }

                // Not code, not blank, not a block command.
                // If a TAG segment is open, multi-line description (including lines starting with inline formatting)
                // continues inside the current TAG segment.
                if (hasActiveSeg && currentSeg.kind == CommentSegmentKind::Tag)
                {
                    currentSeg.lines.push_back(line);
                }
                else
                {
                    if (!hasActiveSeg || currentSeg.kind != CommentSegmentKind::Text)
                    {
                        flushActiveSeg();
                        currentSeg.kind = CommentSegmentKind::Text;
                        hasActiveSeg = true;
                    }
                    currentSeg.lines.push_back(line);
                }
            }

            if (inCode)
            {
                segments.push_back(std::move(currentCode));
            }
            else
            {
                flushActiveSeg();
            }

            return segments;
        }

        /** @brief Wraps lines into a synthetic block comment for parsing. */
        std::string WrapInSyntheticComment(const std::vector<std::string> &lines)
        {
            std::string synthetic = "/**\n";
            for (const auto &l : lines)
            {
                if (l.empty())
                {
                    synthetic += " *\n";
                }
                else
                {
                    synthetic += " * " + l + "\n";
                }
            }
            synthetic += " */";
            return synthetic;
        }

        /** @brief Helper to find a child node with a matching type name. */
        TSNode FindChildByType(TSNode node, const char *typeName)
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                if (std::strcmp(ts_node_type(child), typeName) == 0)
                {
                    return child;
                }
            }
            return TSNode{};
        }

        /**
         * @brief Renders an inline syntax node (code_word, emphasis, link, function_link) to Markdown.
         *
         * Handles the slash forms of \c and \a (which parse as code_word and emphasis), as well
         * as function links and HTML links.
         */
        std::string RenderInlineNode(TSNode node, const std::string &sourceCode)
        {
            const char *type = ts_node_type(node);
            if (std::strcmp(type, "code_word") == 0)
            {
                TSNode codeChild = FindChildByType(node, "code");
                if (!ts_node_is_null(codeChild))
                {
                    return "`" + std::string(parser::DoxygenParser::GetNodeText(codeChild, sourceCode)) + "`";
                }
                std::string_view full = parser::DoxygenParser::GetNodeText(node, sourceCode);
                if (full.starts_with("\\c"))
                {
                    full.remove_prefix(2);
                }
                while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
                {
                    full.remove_prefix(1);
                }
                return "`" + std::string(full) + "`";
            }
            if (std::strcmp(type, "emphasis") == 0)
            {
                TSNode textChild = FindChildByType(node, "text");
                if (!ts_node_is_null(textChild))
                {
                    return "*" + std::string(parser::DoxygenParser::GetNodeText(textChild, sourceCode)) + "*";
                }
                std::string_view full = parser::DoxygenParser::GetNodeText(node, sourceCode);
                if (full.starts_with("\\a"))
                {
                    full.remove_prefix(2);
                }
                while (!full.empty() && (full.front() == ' ' || full.front() == '\t'))
                {
                    full.remove_prefix(1);
                }
                return "*" + std::string(full) + "*";
            }
            if (std::strcmp(type, "function_link") == 0)
            {
                return "`" + std::string(parser::DoxygenParser::GetNodeText(node, sourceCode)) + "`";
            }
            if (std::strcmp(type, "link") == 0)
            {
                std::string nodeText = std::string(parser::DoxygenParser::GetNodeText(node, sourceCode));
                std::string linkText;
                TSNode textChild = FindChildByType(node, "text");
                if (!ts_node_is_null(textChild))
                {
                    linkText = std::string(parser::DoxygenParser::GetNodeText(textChild, sourceCode));
                }

                std::string url;
                size_t hrefPos = nodeText.find("href=");
                if (hrefPos != std::string::npos)
                {
                    size_t quoteStart = hrefPos + 5;
                    if (quoteStart < nodeText.size() && (nodeText[quoteStart] == '"' || nodeText[quoteStart] == '\''))
                    {
                        char q = nodeText[quoteStart];
                        size_t quoteEnd = nodeText.find(q, quoteStart + 1);
                        if (quoteEnd != std::string::npos)
                        {
                            url = nodeText.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
                        }
                    }
                }
                if (!url.empty())
                {
                    if (!linkText.empty())
                    {
                        return "[" + linkText + "](" + url + ")";
                    }
                    return "[" + url + "](" + url + ")";
                }
                return linkText.empty() ? nodeText : linkText;
            }

            return std::string(parser::DoxygenParser::GetNodeText(node, sourceCode));
        }

        /**
         * @brief Renders a description node by walking its byte range and substituting named children.
         *
         * Plain text around named children is anonymous in tree-sitter-doxygen. Concatenating named
         * children loses plain text. We walk the byte range verbatim, substituting named children
         * with their Markdown rendering, then clean line decorations.
         */
        std::string RenderDescription(TSNode descNode, const std::string &sourceCode)
        {
            uint32_t startByte = ts_node_start_byte(descNode);
            uint32_t endByte = ts_node_end_byte(descNode);
            if (startByte >= endByte || endByte > sourceCode.size())
            {
                return "";
            }

            std::string raw;
            uint32_t cursor = startByte;
            uint32_t childCount = ts_node_named_child_count(descNode);

            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_named_child(descNode, i);
                uint32_t childStart = ts_node_start_byte(child);
                uint32_t childEnd = ts_node_end_byte(child);

                if (childStart > cursor)
                {
                    std::string verbatim = sourceCode.substr(cursor, childStart - cursor);
                    raw.append(RewriteAtInlineCommands(verbatim));
                }

                raw.append(RenderInlineNode(child, sourceCode));
                cursor = std::max(cursor, childEnd);
            }

            if (cursor < endByte)
            {
                std::string verbatim = sourceCode.substr(cursor, endByte - cursor);
                raw.append(RewriteAtInlineCommands(verbatim));
            }

            return CleanDescriptionLines(raw);
        }

        /**
         * @brief Extracts the brief text from a brief_header node.
         *
         * Reads brief_header whether or not a tag_name child (@brief or \brief) is present,
         * supporting both explicit @brief and Doxygen AUTOBRIEF.
         */
        std::string ProcessBriefHeader(TSNode briefNode, const std::string &sourceCode)
        {
            TSNode descChild = FindChildByType(briefNode, "brief_description");
            if (!ts_node_is_null(descChild))
            {
                return RenderDescription(descChild, sourceCode);
            }

            TSNode tagChild = FindChildByType(briefNode, "tag_name");
            uint32_t start = !ts_node_is_null(tagChild) ? ts_node_end_byte(tagChild) : ts_node_start_byte(briefNode);
            uint32_t end = ts_node_end_byte(briefNode);
            if (end > start && end <= sourceCode.size())
            {
                return CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
            }

            return "";
        }

        /**
         * @brief Formats a code_block node into a Markdown fenced block.
         *
         * Extracts optional language from code_block_language, strips leading ' * ' line decorations
         * from code_block_content, and emits a fenced block.
         */
        std::string ProcessCodeBlock(TSNode blockNode, const std::string &sourceCode)
        {
            TSNode langNode = FindChildByType(blockNode, "code_block_language");
            std::string lang;
            if (!ts_node_is_null(langNode))
            {
                lang = std::string(parser::DoxygenParser::GetNodeText(langNode, sourceCode));
                lang = Trim(lang);
                if (lang.starts_with('.'))
                {
                    lang = lang.substr(1);
                }
                if (lang.starts_with('{') && lang.ends_with('}'))
                {
                    lang = lang.substr(1, lang.size() - 2);
                }
                if (lang.starts_with('.'))
                {
                    lang = lang.substr(1);
                }
                lang = Trim(lang);
            }

            TSNode contentNode = FindChildByType(blockNode, "code_block_content");
            std::string content;
            if (!ts_node_is_null(contentNode))
            {
                content = std::string(parser::DoxygenParser::GetNodeText(contentNode, sourceCode));
            }

            auto lines = SplitLines(content);
            for (auto &line : lines)
            {
                line = StripLineLeadingStar(line);
            }

            while (!lines.empty() && Trim(lines.front()).empty())
            {
                lines.erase(lines.begin());
            }
            while (!lines.empty() && Trim(lines.back()).empty())
            {
                lines.pop_back();
            }

            std::string joined;
            for (size_t i = 0; i < lines.size(); ++i)
            {
                if (i > 0)
                {
                    joined += "\n";
                }
                joined += lines[i];
            }

            return "```" + lang + "\n" + joined + "\n```";
        }

        struct ParamItem
        {
            std::vector<std::string> names;
            std::string direction; // "in", "out", "in,out", or empty
            std::string description;
        };

        struct TParamItem
        {
            std::string name;
            std::string description;
        };

        enum class DocBlockKind
        {
            Brief,
            Body,
            CodeBlock,
            TParam,
            Param,
            Return,
            Admonition
        };

        struct DocBlock
        {
            DocBlockKind kind;
            std::string text;
            std::string label;
            TParamItem tparam;
            ParamItem param;

            static DocBlock MakeBrief(std::string t)
            {
                DocBlock b;
                b.kind = DocBlockKind::Brief;
                b.text = std::move(t);
                return b;
            }

            static DocBlock MakeBody(std::string t)
            {
                DocBlock b;
                b.kind = DocBlockKind::Body;
                b.text = std::move(t);
                return b;
            }

            static DocBlock MakeCodeBlock(std::string t)
            {
                DocBlock b;
                b.kind = DocBlockKind::CodeBlock;
                b.text = std::move(t);
                return b;
            }

            static DocBlock MakeTParam(TParamItem tp)
            {
                DocBlock b;
                b.kind = DocBlockKind::TParam;
                b.tparam = std::move(tp);
                return b;
            }

            static DocBlock MakeParam(ParamItem p)
            {
                DocBlock b;
                b.kind = DocBlockKind::Param;
                b.param = std::move(p);
                return b;
            }

            static DocBlock MakeReturn(std::string t)
            {
                DocBlock b;
                b.kind = DocBlockKind::Return;
                b.text = std::move(t);
                return b;
            }

            static DocBlock MakeAdmonition(std::string l, std::string t)
            {
                DocBlock b;
                b.kind = DocBlockKind::Admonition;
                b.label = std::move(l);
                b.text = std::move(t);
                return b;
            }
        };

        /**
         * @brief Extracts parameter names, optional direction, and description from a @param tag.
         *
         * Handles multiple comma-separated identifiers, strips brackets from storageclass,
         * and normalizes both "inout" and real Doxygen "[in,out]" (which parses as ERROR) to "in,out".
         */
        ParamItem ProcessParamTag(TSNode tagNode, const std::string &sourceCode)
        {
            ParamItem item;
            uint32_t childCount = ts_node_child_count(tagNode);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(tagNode, i);
                const char *type = ts_node_type(child);
                if (std::strcmp(type, "storageclass") == 0)
                {
                    std::string rawSc = std::string(parser::DoxygenParser::GetNodeText(child, sourceCode));
                    size_t open = rawSc.find('[');
                    size_t close = rawSc.rfind(']');
                    if (open != std::string::npos && close != std::string::npos && close > open)
                    {
                        std::string inner = Trim(rawSc.substr(open + 1, close - open - 1));
                        std::string lowerInner = ToLower(inner);
                        std::string compact;
                        for (char c : lowerInner)
                        {
                            if (c != ' ' && c != '\t')
                            {
                                compact += c;
                            }
                        }
                        if (compact == "inout" || compact == "in,out")
                        {
                            item.direction = "in,out";
                        }
                        else if (compact == "in")
                        {
                            item.direction = "in";
                        }
                        else if (compact == "out")
                        {
                            item.direction = "out";
                        }
                        else
                        {
                            item.direction = compact;
                        }
                    }
                }
                else if (std::strcmp(type, "identifier") == 0 ||
                         std::strcmp(type, "qualified_identifier") == 0)
                {
                    std::string name = std::string(parser::DoxygenParser::GetNodeText(child, sourceCode));
                    name = Trim(name);
                    if (!name.empty())
                    {
                        item.names.push_back(std::move(name));
                    }
                }
                else if (std::strcmp(type, "description") == 0)
                {
                    item.description = RenderDescription(child, sourceCode);
                }
            }

            if (item.names.empty() && !item.description.empty())
            {
                size_t sp = item.description.find_first_of(" \t");
                if (sp != std::string::npos)
                {
                    item.names.push_back(item.description.substr(0, sp));
                    item.description = Trim(item.description.substr(sp + 1));
                }
                else
                {
                    item.names.push_back(item.description);
                    item.description = "";
                }
            }

            return item;
        }

        /**
         * @brief Splits the template parameter name from the description for a @tparam tag.
         *
         * Tree-sitter-doxygen provides no structured name child for @tparam; the parameter name
         * is extracted as the first whitespace-delimited word of the description.
         */
        TParamItem ProcessTParamTag(TSNode tagNode, const std::string &sourceCode)
        {
            TParamItem item;
            TSNode descChild = FindChildByType(tagNode, "description");
            std::string desc;
            if (!ts_node_is_null(descChild))
            {
                desc = RenderDescription(descChild, sourceCode);
            }
            else
            {
                TSNode nameChild = FindChildByType(tagNode, "tag_name");
                uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
                uint32_t end = ts_node_end_byte(tagNode);
                if (end > start && end <= sourceCode.size())
                {
                    desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
                }
            }

            desc = Trim(desc);
            size_t sp = desc.find_first_of(" \t");
            if (sp != std::string::npos)
            {
                item.name = desc.substr(0, sp);
                item.description = Trim(desc.substr(sp + 1));
            }
            else
            {
                item.name = desc;
                item.description = "";
            }
            return item;
        }

        /** @brief Extracts the description string for return tags. */
        std::string ProcessReturnTag(TSNode tagNode, const std::string &sourceCode)
        {
            TSNode descChild = FindChildByType(tagNode, "description");
            if (!ts_node_is_null(descChild))
            {
                return RenderDescription(descChild, sourceCode);
            }
            return "";
        }

        /**
         * @brief Maps tag name and description to an Admonition block.
         *
         * Applies canonical clangd label renamings (see/sa -> See also, throw/throws/exception -> Throws)
         * and capitalizes any unknown tags rather than silently dropping them.
         */
        DocBlock ProcessAdmonitionTag(const std::string &cleanedTag, TSNode tagNode, const std::string &sourceCode)
        {
            std::string desc;
            TSNode descChild = FindChildByType(tagNode, "description");
            if (!ts_node_is_null(descChild))
            {
                desc = RenderDescription(descChild, sourceCode);
            }
            else
            {
                std::vector<std::string> parts;
                uint32_t count = ts_node_child_count(tagNode);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode c = ts_node_child(tagNode, i);
                    const char *ct = ts_node_type(c);
                    if (std::strcmp(ct, "identifier") == 0 ||
                        std::strcmp(ct, "function_link") == 0 ||
                        std::strcmp(ct, "function") == 0)
                    {
                        parts.push_back(std::string(parser::DoxygenParser::GetNodeText(c, sourceCode)));
                    }
                }
                if (!parts.empty())
                {
                    for (size_t i = 0; i < parts.size(); ++i)
                    {
                        if (i > 0)
                        {
                            desc += ", ";
                        }
                        desc += parts[i];
                    }
                }
                else
                {
                    TSNode nameChild = FindChildByType(tagNode, "tag_name");
                    uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
                    uint32_t end = ts_node_end_byte(tagNode);
                    if (end > start && end <= sourceCode.size())
                    {
                        desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
                    }
                }
            }
            desc = Trim(desc);

            std::string lower = ToLower(cleanedTag);
            std::string label;
            if (lower == "note")
                label = "Note";
            else if (lower == "warning")
                label = "Warning";
            else if (lower == "attention")
                label = "Attention";
            else if (lower == "deprecated")
                label = "Deprecated";
            else if (lower == "see" || lower == "sa")
                label = "See also";
            else if (lower == "remark" || lower == "remarks")
                label = "Remark";
            else if (lower == "since")
                label = "Since";
            else if (lower == "todo")
                label = "Todo";
            else if (lower == "pre")
                label = "Pre";
            else if (lower == "post")
                label = "Post";
            else if (lower == "throw" || lower == "throws" || lower == "exception")
                label = "Throws";
            else
            {
                char first = static_cast<char>(std::toupper(static_cast<unsigned char>(cleanedTag[0])));
                label = first + cleanedTag.substr(1);
            }

            return DocBlock::MakeAdmonition(std::move(label), std::move(desc));
        }

        /** @brief Appends text to a string block with proper spacing. */
        void AppendToBlockText(std::string &target, const std::string &addition)
        {
            if (addition.empty())
            {
                return;
            }
            if (target.empty())
            {
                target = addition;
                return;
            }
            if (!target.ends_with(' ') && !target.ends_with('\t') &&
                !target.ends_with('(') && !target.ends_with('[') && !target.ends_with('{'))
            {
                target += ' ';
            }
            target += addition;
        }

        /** @brief Appends text to a DocBlock target. */
        void AppendToDocBlock(DocBlock &block, const std::string &text)
        {
            switch (block.kind)
            {
            case DocBlockKind::Brief:
            case DocBlockKind::Body:
            case DocBlockKind::Return:
            case DocBlockKind::Admonition:
                AppendToBlockText(block.text, text);
                break;
            case DocBlockKind::TParam:
                AppendToBlockText(block.tparam.description, text);
                break;
            case DocBlockKind::Param:
                AppendToBlockText(block.param.description, text);
                break;
            case DocBlockKind::CodeBlock:
                break;
            }
        }

        /**
         * @brief Merges an inline formatting tag (\b, \e, \em, \a, \c, \p) into the preceding block.
         *
         * The Doxygen grammar treats any backslash or at word as a tag, fragmenting lines at inline
         * formatting commands. This merge pass appends the rendered inline command to the preceding block
         * (brief, body paragraph, parameter description, return, or admonition) rather than allowing it
         * to become an admonition.
         */
        void MergeInlineTag(const std::string &cmd, TSNode tagNode, const std::string &sourceCode, std::vector<DocBlock> &blocks)
        {
            TSNode descChild = FindChildByType(tagNode, "description");
            std::string desc;
            if (!ts_node_is_null(descChild))
            {
                desc = RenderDescription(descChild, sourceCode);
            }
            else
            {
                TSNode nameChild = FindChildByType(tagNode, "tag_name");
                uint32_t start = !ts_node_is_null(nameChild) ? ts_node_end_byte(nameChild) : ts_node_start_byte(tagNode);
                uint32_t end = ts_node_end_byte(tagNode);
                if (end > start && end <= sourceCode.size())
                {
                    desc = CleanDescriptionLines(RewriteAtInlineCommands(sourceCode.substr(start, end - start)));
                }
            }

            std::string trimmedDesc = TrimLeading(desc);
            std::string X;
            std::string remainder;
            size_t sp = trimmedDesc.find_first_of(" \t");
            if (sp != std::string::npos)
            {
                X = trimmedDesc.substr(0, sp);
                remainder = trimmedDesc.substr(sp);
            }
            else
            {
                X = trimmedDesc;
                remainder = "";
            }

            std::string formatted;
            if (cmd == "c" || cmd == "p")
            {
                formatted = "`" + X + "`";
            }
            else if (cmd == "b")
            {
                formatted = "**" + X + "**";
            }
            else if (cmd == "a" || cmd == "e" || cmd == "em")
            {
                formatted = "*" + X + "*";
            }
            else
            {
                formatted = X;
            }

            std::string inlineRendered = formatted + remainder;

            if (blocks.empty() || blocks.back().kind == DocBlockKind::CodeBlock)
            {
                blocks.push_back(DocBlock::MakeBody(std::move(inlineRendered)));
            }
            else
            {
                AppendToDocBlock(blocks.back(), inlineRendered);
            }
        }
        /**
         * @brief Parses a single synthetic document segment and collects AST DocBlocks.
         */
        void ParseDocSegment(const std::string &syntheticDoc, std::vector<DocBlock> &blocks)
        {
            if (Trim(syntheticDoc).empty())
            {
                return;
            }

            parser::DoxygenParser parser;
            TSTree *tree = parser.Parse(syntheticDoc);
            if (!tree)
            {
                return;
            }

            TSNode root = ts_tree_root_node(tree);
            if (ts_node_is_null(root))
            {
                ts_tree_delete(tree);
                return;
            }

            uint32_t childCount = ts_node_child_count(root);

            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(root, i);
                const char *type = ts_node_type(child);

                if (std::strcmp(type, "brief_header") == 0)
                {
                    std::string brief = ProcessBriefHeader(child, syntheticDoc);
                    if (!brief.empty())
                    {
                        blocks.push_back(DocBlock::MakeBody(std::move(brief)));
                    }
                }
                else if (std::strcmp(type, "description") == 0)
                {
                    std::string body = RenderDescription(child, syntheticDoc);
                    if (!body.empty())
                    {
                        blocks.push_back(DocBlock::MakeBody(std::move(body)));
                    }
                }
                else if (std::strcmp(type, "code_block") == 0)
                {
                    std::string code = ProcessCodeBlock(child, syntheticDoc);
                    if (!code.empty())
                    {
                        blocks.push_back(DocBlock::MakeCodeBlock(std::move(code)));
                    }
                }
                else if (std::strcmp(type, "tag") == 0)
                {
                    TSNode nameNode = FindChildByType(child, "tag_name");
                    std::string rawTagName;
                    if (!ts_node_is_null(nameNode))
                    {
                        rawTagName = std::string(parser::DoxygenParser::GetNodeText(nameNode, syntheticDoc));
                    }
                    else
                    {
                        TSNode scNode = FindChildByType(child, "storageclass");
                        if (!ts_node_is_null(scNode))
                        {
                            rawTagName = "param";
                        }
                    }

                    if (rawTagName.starts_with('@') || rawTagName.starts_with('\\'))
                    {
                        rawTagName = rawTagName.substr(1);
                    }

                    std::string lowerTag = ToLower(rawTagName);
                    if (lowerTag == "b" || lowerTag == "e" || lowerTag == "em" ||
                        lowerTag == "a" || lowerTag == "c" || lowerTag == "p")
                    {
                        MergeInlineTag(lowerTag, child, syntheticDoc, blocks);
                    }
                    else if (lowerTag == "param")
                    {
                        blocks.push_back(DocBlock::MakeParam(ProcessParamTag(child, syntheticDoc)));
                    }
                    else if (lowerTag == "tparam")
                    {
                        blocks.push_back(DocBlock::MakeTParam(ProcessTParamTag(child, syntheticDoc)));
                    }
                    else if (lowerTag == "return" || lowerTag == "returns" || lowerTag == "result")
                    {
                        blocks.push_back(DocBlock::MakeReturn(ProcessReturnTag(child, syntheticDoc)));
                    }
                    else if (lowerTag == "brief")
                    {
                        std::string brief = ProcessReturnTag(child, syntheticDoc);
                        if (!brief.empty())
                        {
                            blocks.push_back(DocBlock::MakeBody(std::move(brief)));
                        }
                    }
                    else if (!rawTagName.empty())
                    {
                        blocks.push_back(ProcessAdmonitionTag(rawTagName, child, syntheticDoc));
                    }
                }
                else if (std::strcmp(type, "_text_line") == 0)
                {
                    std::string line = std::string(parser::DoxygenParser::GetNodeText(child, syntheticDoc));
                    line = CleanDescriptionLines(RewriteAtInlineCommands(line));
                    if (!line.empty())
                    {
                        if (!blocks.empty() && blocks.back().kind == DocBlockKind::Body)
                        {
                            AppendToBlockText(blocks.back().text, line);
                        }
                        else
                        {
                            blocks.push_back(DocBlock::MakeBody(std::move(line)));
                        }
                    }
                }
            }

            ts_tree_delete(tree);
        }

        /**
         * @brief Assembles extracted blocks and brief into canonical clangd Markdown format.
         */
        std::string AssembleMarkdown(std::string briefText, const std::vector<DocBlock> &blocks)
        {
            // Assembly into canonical clangd order:
            // 1. Brief
            // 2. Body paragraphs and code blocks, in source order
            // 3. \tparam bullets
            // 4. \param bullets
            // 5. Returns
            // 6. Admonitions, in source order
            std::vector<std::string> outputSections;

            // 1. Brief
            size_t firstBriefBlockIdx = std::string::npos;
            if (!briefText.empty())
            {
                outputSections.push_back(briefText);
            }
            else
            {
                for (size_t i = 0; i < blocks.size(); ++i)
                {
                    if (blocks[i].kind == DocBlockKind::Brief && !blocks[i].text.empty())
                    {
                        outputSections.push_back(blocks[i].text);
                        firstBriefBlockIdx = i;
                        break;
                    }
                }
            }

            // 2. Body paragraphs and code blocks in source order
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                if (i == firstBriefBlockIdx)
                {
                    continue;
                }
                const auto &b = blocks[i];
                if ((b.kind == DocBlockKind::Body || b.kind == DocBlockKind::Brief) && !b.text.empty())
                {
                    outputSections.push_back(b.text);
                }
                else if (b.kind == DocBlockKind::CodeBlock && !b.text.empty())
                {
                    outputSections.push_back(b.text);
                }
            }

            // 3. \tparam bullets
            std::vector<std::string> tparamBullets;
            for (const auto &b : blocks)
            {
                if (b.kind == DocBlockKind::TParam)
                {
                    std::string bullet = "* `" + b.tparam.name + "`";
                    if (!b.tparam.description.empty())
                    {
                        bullet += ": " + b.tparam.description;
                    }
                    tparamBullets.push_back(std::move(bullet));
                }
            }
            if (!tparamBullets.empty())
            {
                std::string joined;
                for (size_t i = 0; i < tparamBullets.size(); ++i)
                {
                    if (i > 0)
                    {
                        joined += "\n";
                    }
                    joined += tparamBullets[i];
                }
                outputSections.push_back(std::move(joined));
            }

            // 4. \param bullets
            std::vector<std::string> paramBullets;
            for (const auto &b : blocks)
            {
                if (b.kind == DocBlockKind::Param)
                {
                    for (const auto &name : b.param.names)
                    {
                        std::string bullet = "* `" + name + "`";
                        if (!b.param.direction.empty())
                        {
                            bullet += " *(" + b.param.direction + ")*";
                        }
                        if (!b.param.description.empty())
                        {
                            bullet += ": " + b.param.description;
                        }
                        paramBullets.push_back(std::move(bullet));
                    }
                }
            }
            if (!paramBullets.empty())
            {
                std::string joined;
                for (size_t i = 0; i < paramBullets.size(); ++i)
                {
                    if (i > 0)
                    {
                        joined += "\n";
                    }
                    joined += paramBullets[i];
                }
                outputSections.push_back(std::move(joined));
            }

            // 5. Returns
            for (const auto &b : blocks)
            {
                if (b.kind == DocBlockKind::Return)
                {
                    std::string ret = "**Returns:**";
                    if (!b.text.empty())
                    {
                        ret += " " + b.text;
                    }
                    outputSections.push_back(std::move(ret));
                }
            }

            // 6. Admonitions in source order
            for (const auto &b : blocks)
            {
                if (b.kind == DocBlockKind::Admonition)
                {
                    std::string adm = "> **" + b.label + ":**";
                    if (!b.text.empty())
                    {
                        adm += " " + b.text;
                    }
                    outputSections.push_back(std::move(adm));
                }
            }

            // Join sections separated by a blank line (\n\n)
            std::string finalMarkdown;
            for (size_t i = 0; i < outputSections.size(); ++i)
            {
                if (i > 0)
                {
                    finalMarkdown += "\n\n";
                }
                finalMarkdown += outputSections[i];
            }

            return Trim(finalMarkdown);
        }
    }

    std::string RenderDoxygenMarkdown(const std::string &rawComment)
    {
        auto contentLines = ExtractCommentLines(rawComment);
        if (contentLines.empty() || std::all_of(contentLines.begin(), contentLines.end(), [](const std::string &l)
                                                { return Trim(l).empty(); }))
        {
            return "";
        }

        std::string briefText = ExtractBrief(contentLines);

        auto segments = SegmentCommentLines(contentLines);

        std::vector<DocBlock> allBlocks;

        for (auto &seg : segments)
        {
            if (seg.kind == CommentSegmentKind::VerbatimCode)
            {
                while (!seg.lines.empty() && Trim(seg.lines.front()).empty())
                {
                    seg.lines.erase(seg.lines.begin());
                }
                while (!seg.lines.empty() && Trim(seg.lines.back()).empty())
                {
                    seg.lines.pop_back();
                }

                std::string joined;
                for (size_t i = 0; i < seg.lines.size(); ++i)
                {
                    if (i > 0)
                    {
                        joined += "\n";
                    }
                    joined += seg.lines[i];
                }

                std::string fenced = "```" + seg.language + "\n";
                if (!joined.empty())
                {
                    fenced += joined + "\n";
                }
                fenced += "```";
                allBlocks.push_back(DocBlock::MakeCodeBlock(std::move(fenced)));
            }
            else
            {
                if (seg.lines.empty())
                {
                    continue;
                }
                std::string synthetic = WrapInSyntheticComment(seg.lines);
                ParseDocSegment(synthetic, allBlocks);
            }
        }

        return AssembleMarkdown(std::move(briefText), allBlocks);
    }
}
