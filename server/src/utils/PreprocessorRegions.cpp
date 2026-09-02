#include "utils/PreprocessorRegions.h"

#include <cctype>
#include <utility>

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

        /**
         * @brief Walks a document and reports every directive that starts a line.
         *
         * Comments and string literals are skipped so a `#if` written inside one is not mistaken
         * for a directive - the same discipline IncludeResolver::ExtractIncludes applies.
         *
         * Shared rather than written twice: reading `#define` out of a predefined stub needs
         * exactly this walk, and a second copy of it would drift from this one the first time
         * either learned about a new kind of literal.
         *
         * @param fn Called as fn(line, name, argument) for each `#name argument`, where argument is
         *        the identifier that follows the name, empty when there is none. Both are views
         *        into @p sourceCode.
         * @return The 0-based number of the last line scanned, for a directive left unclosed.
         */
        template <typename Fn>
        uint32_t ForEachDirective(std::string_view sourceCode, Fn &&fn)
        {
            const size_t n = sourceCode.size();
            size_t i = 0;
            uint32_t currentLine = 0;
            bool atLineStart = true;

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
                    const std::string_view name = sourceCode.substr(nameStart, i - nameStart);

                    while (i < n && (sourceCode[i] == ' ' || sourceCode[i] == '\t'))
                        ++i;

                    const size_t argStart = i;
                    while (i < n && IsIdentifierChar(sourceCode[i]))
                        ++i;
                    const std::string_view argument = sourceCode.substr(argStart, i - argStart);

                    fn(directiveLine, name, argument);

                    while (i < n && sourceCode[i] != '\n' && sourceCode[i] != '\r')
                        ++i;
                    continue;
                }

                atLineStart = false;
                ++i;
            }

            return currentLine;
        }
    }

    std::vector<ExcludedLineRange> FindExcludedLineRanges(
        std::string_view sourceCode,
        const ankerl::unordered_dense::set<std::string> &definedWords)
    {
        std::vector<ExcludedLineRange> ranges;

        std::vector<OpenDirective> stack;

        // Depth of `#if`s opened *inside* an already-excluded block. They are not evaluated - the
        // whole region is going away regardless - but they have to be counted so the matching
        // `#endif` of the outer directive is the one that closes it.
        int excludedNesting = 0;

        const uint32_t lastLine = ForEachDirective(
            sourceCode,
            [&](uint32_t directiveLine, std::string_view directive, std::string_view word) {
                const bool atExcludedTop = !stack.empty() && stack.back().excluded;

                if (directive == "if")
                {
                    if (atExcludedTop)
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
            });

        // An `#if` never closed runs to the end of the file, which is how the compiler treats it.
        for (const auto &open : stack)
        {
            if (open.excluded)
                ranges.push_back(ExcludedLineRange{ open.line, lastLine });
        }

        return ranges;
    }

    std::vector<std::string> ScanDefinedWords(std::string_view sourceCode)
    {
        std::vector<std::string> words;

        ForEachDirective(sourceCode,
                         [&](uint32_t, std::string_view directive, std::string_view word) {
                             if (directive == "define" && !word.empty())
                                 words.emplace_back(word);
                         });

        return words;
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
