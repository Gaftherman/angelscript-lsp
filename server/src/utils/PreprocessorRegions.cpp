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

        /**
         * @brief One `#if <word>` that has been opened and is waiting for its `#endif`.
         *
         * With no host extensions enabled there is exactly one branch, so `branchStart` is the
         * `#if` line and `takenAlready` never changes - which is what makes the stock path fall out
         * of the same code rather than needing its own.
         */
        struct OpenDirective
        {
            uint32_t branchStart = 0;   ///< Line of the directive that opened the current branch.
            bool excluded = false;      ///< True when the current branch is the dropped one.
            bool takenAlready = false;  ///< True once some branch of this `#if` has been live.
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
        const ankerl::unordered_dense::set<std::string> &definedWords,
        const PreprocessorFeatures &features)
    {
        std::vector<ExcludedLineRange> ranges;

        std::vector<OpenDirective> stack;

        // Depth of `#if`s opened *inside* an already-excluded block. They are not evaluated - the
        // whole region is going away regardless - but they have to be counted so the matching
        // `#endif` of the outer directive is the one that closes it.
        int excludedNesting = 0;

        // Only ever written when features.defineInScripts is on. A `#define` in a script is a
        // syntax error to the stock add-on, so without that switch this stays a view of the
        // caller's set and a `#define` line means nothing here.
        ankerl::unordered_dense::set<std::string> localWords;
        bool usingLocalWords = false;

        const auto isDefined = [&](std::string_view word) {
            const std::string key(word);
            return usingLocalWords ? localWords.contains(key) : definedWords.contains(key);
        };

        // Closes the branch that was open and opens the next one at the same line. `#endif` uses it
        // too, with nothing following, which is why the dead-range bookkeeping lives in one place.
        const auto closeBranch = [&](OpenDirective &open, uint32_t boundaryLine) {
            if (open.excluded)
            {
                // Inclusive of both directive lines: CScriptBuilder blanks those too.
                ranges.push_back(ExcludedLineRange{ open.branchStart, boundaryLine });
            }
            else
            {
                open.takenAlready = true;
            }
        };

        const uint32_t lastLine = ForEachDirective(
            sourceCode,
            [&](uint32_t directiveLine, std::string_view directive, std::string_view word) {
                const bool atExcludedTop = !stack.empty() && stack.back().excluded;

                const bool opensRegion =
                    directive == "if" ||
                    (features.ifdefSupport && (directive == "ifdef" || directive == "ifndef"));

                if (opensRegion)
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
                        const bool defined = isDefined(word);
                        const bool live = directive == "ifndef" ? !defined : defined;

                        OpenDirective open;
                        open.branchStart = directiveLine;
                        open.excluded = !live;
                        stack.push_back(open);
                    }
                }
                else if (features.defineInScripts && directive == "define" && !word.empty() &&
                         !atExcludedTop && excludedNesting == 0)
                {
                    // Copied lazily, and only once: most documents have no `#define` at all, and
                    // the ones that do should not pay for a set copy per directive.
                    if (!usingLocalWords)
                    {
                        localWords = definedWords;
                        usingLocalWords = true;
                    }
                    localWords.insert(std::string(word));
                }
                else if ((features.elseSupport && directive == "else") ||
                         (features.elifSupport && directive == "elif"))
                {
                    // A branch boundary inside a nested dead region is part of that region, not of
                    // the directive this one belongs to.
                    if (excludedNesting > 0 || stack.empty())
                        return;

                    OpenDirective &open = stack.back();
                    closeBranch(open, directiveLine);

                    // `#else` is unconditional; `#elif` still has to be true. Either way a branch
                    // after one that was already taken is dead - that is the whole of the rule.
                    const bool conditionHolds = directive == "else" || (!word.empty() && isDefined(word));

                    open.branchStart = directiveLine;
                    open.excluded = open.takenAlready || !conditionHolds;
                }
                else if (directive == "endif")
                {
                    if (excludedNesting > 0)
                    {
                        --excludedNesting;
                    }
                    else if (!stack.empty())
                    {
                        OpenDirective open = stack.back();
                        stack.pop_back();
                        closeBranch(open, directiveLine);
                    }
                }
            });

        // An `#if` never closed runs to the end of the file, which is how the compiler treats it.
        for (const auto &open : stack)
        {
            if (open.excluded)
                ranges.push_back(ExcludedLineRange{ open.branchStart, lastLine });
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
