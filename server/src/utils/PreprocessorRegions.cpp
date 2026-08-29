#include "utils/PreprocessorRegions.h"

#include <cctype>

namespace angel_lsp::utils
{
    namespace
    {
        bool IsIdentifierChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        /** @brief One `#if <word>` that has been opened and is waiting for its `#endif`. */
        struct OpenDirective
        {
            uint32_t line = 0;
            bool excluded = false;  ///< True when the word was not defined, so the body is dropped.
        };
    }

    std::vector<ExcludedLineRange> FindExcludedLineRanges(
        std::string_view sourceCode,
        const ankerl::unordered_dense::set<std::string> &definedWords)
    {
        std::vector<ExcludedLineRange> ranges;

        const size_t n = sourceCode.size();
        size_t i = 0;
        uint32_t currentLine = 0;
        bool atLineStart = true;

        std::vector<OpenDirective> stack;

        // Depth of `#if`s opened *inside* an already-excluded block. They are not evaluated - the
        // whole region is going away regardless - but they have to be counted so the matching
        // `#endif` of the outer directive is the one that closes it.
        int excludedNesting = 0;

        const auto atExcludedTop = [&]() {
            return !stack.empty() && stack.back().excluded;
        };

        while (i < n)
        {
            const char c = sourceCode[i];

            if (c == '\r')
            {
                if (i + 1 < n && sourceCode[i + 1] == '\n')
                    ++i;
                ++currentLine;
                atLineStart = true;
                ++i;
                continue;
            }
            if (c == '\n')
            {
                ++currentLine;
                atLineStart = true;
                ++i;
                continue;
            }
            if (c == ' ' || c == '\t')
            {
                ++i;
                continue;
            }

            // Comments and literals are skipped so a `#if` written inside one is not mistaken for a
            // directive - the same discipline IncludeResolver::ExtractIncludes applies.
            if (c == '/' && i + 1 < n && sourceCode[i + 1] == '/')
            {
                i += 2;
                while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                    ++i;
                continue;
            }
            if (c == '/' && i + 1 < n && sourceCode[i + 1] == '*')
            {
                i += 2;
                while (i < n)
                {
                    if (sourceCode[i] == '\n')
                    {
                        ++currentLine;
                        atLineStart = true;
                    }
                    else if (sourceCode[i] == '*' && i + 1 < n && sourceCode[i + 1] == '/')
                    {
                        i += 2;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (c == '"' || c == '\'')
            {
                const char quote = c;
                ++i;
                while (i < n)
                {
                    if (sourceCode[i] == '\\')
                    {
                        i += 2;
                        continue;
                    }
                    if (sourceCode[i] == quote)
                    {
                        ++i;
                        break;
                    }
                    if (sourceCode[i] == '\n' || sourceCode[i] == '\r')
                        break;
                    ++i;
                }
                continue;
            }

            if (c == '#' && atLineStart)
            {
                const uint32_t directiveLine = currentLine;
                ++i;

                while (i < n && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
                    ++i;

                const size_t nameStart = i;
                while (i < n && IsIdentifierChar(sourceCode[i]))
                    ++i;
                const std::string_view directive = sourceCode.substr(nameStart, i - nameStart);

                if (directive == "if")
                {
                    while (i < n && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
                        ++i;

                    const size_t wordStart = i;
                    while (i < n && IsIdentifierChar(sourceCode[i]))
                        ++i;
                    const std::string_view word = sourceCode.substr(wordStart, i - wordStart);

                    if (atExcludedTop())
                    {
                        // Inside a region already being dropped; only the nesting count matters.
                        ++excludedNesting;
                    }
                    else if (word.empty())
                    {
                        // `#if` with no identifier is not a directive CScriptBuilder recognises, so
                        // it excludes nothing and does not open a region either.
                    }
                    else
                    {
                        OpenDirective open;
                        open.line = directiveLine;
                        open.excluded = !definedWords.contains(std::string(word));
                        stack.push_back(open);
                    }
                }
                else if (directive == "endif")
                {
                    if (excludedNesting > 0)
                    {
                        --excludedNesting;
                    }
                    else if (!stack.empty())
                    {
                        const OpenDirective open = stack.back();
                        stack.pop_back();

                        if (open.excluded)
                        {
                            // Inclusive of both directive lines: CScriptBuilder blanks those too.
                            ranges.push_back(ExcludedLineRange{ open.line, directiveLine });
                        }
                    }
                }

                while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                    ++i;
                continue;
            }

            atLineStart = false;
            ++i;
        }

        // An `#if` never closed runs to the end of the file, which is how the compiler treats it.
        for (const auto &open : stack)
        {
            if (open.excluded)
                ranges.push_back(ExcludedLineRange{ open.line, currentLine });
        }

        return ranges;
    }

    bool IsLineExcluded(const std::vector<ExcludedLineRange> &ranges, uint32_t line)
    {
        for (const auto &range : ranges)
        {
            if (line >= range.startLine && line <= range.endLine)
                return true;
        }
        return false;
    }
}
