#pragma once

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
/**
 * @brief A span of lines the preprocessor removes before the compiler sees them.
 *        Inclusive on both ends, and covering the `#if` and `#endif` lines themselves - CScriptBuilder
 */
struct ExcludedLineRange
{
    uint32_t startLine = 0;
    uint32_t endLine = 0;
};

/**
 * @brief Preprocessor extensions a host has added to CScriptBuilder, all off by default.
 *        None of these exist in the stock add-on - each was measured against the real compiler and
 */
struct PreprocessorFeatures
{
    /// `#else` opens the complementary branch instead of being swallowed by the dead block.
    bool elseSupport = false;

    /// `#elif <word>` opens another branch. Implies the `#else` branch model.
    bool elifSupport = false;

    /// `#ifdef` / `#ifndef` open a region, the second with the condition negated.
    bool ifdefSupport = false;

    /// `#define <word>` inside a script defines the word from that line on.
    bool defineInScripts = false;
};

/**
 * @brief Identifies source lines excluded by `#if` / `#endif` preprocessor directives.
 * @param[in] sourceCode Document text buffer to scan for directives.
 * @param[in] definedWords Set of preprocessor macro words defined by the host application.
 * @param[in] features Optional preprocessor dialect extensions supported by the host.
 * @return Sorted non-overlapping list of excluded line ranges.
 * @note Preserves newlines across blanked regions to keep line numbering synchronized.
 */
std::vector<ExcludedLineRange>
FindExcludedLineRanges(std::string_view sourceCode, const ankerl::unordered_dense::set<std::string>& definedWords = {},
                       const PreprocessorFeatures& features = {});

/**
 * @brief What is wrong with a directive, which decides what the reader is told.
 */
enum class DirectiveProblem : uint8_t
{
    /**
     * @brief A directive CScriptBuilder knows about, that this host's copy does not support.
     *        The fix is a setting: the host may have patched the add-on, and the reader is the only
     */
    Unsupported,

    /**
     * @brief Not a directive at all - a misspelled name, or no name.
     *        Measured: `#incude "test"` is `ERROR (1, 1): Unexpected token '<unrecognized token>'`.
     */
    Unrecognised,

    /**
     * @brief A recognised name, with whitespace between it and the `#`.
     *        Measured, and the surprise of this pass: `# include "helper.as"` and `#  if FOO` are both
     */
    SpaceAfterHash,

    /**
     * @brief `#include` spelled correctly, with a path that is not in double quotes.
     *        Measured: `#include helper.as` is `Unexpected token '<unrecognized token>'`, the quoted
     */
    IncludeNotQuoted,
};

struct UnsupportedDirective
{
    uint32_t line = 0;
    uint32_t startColumn = 0; ///< Byte column of the `#`.
    uint32_t endColumn = 0;   ///< One past the last character of the directive name.
    DirectiveProblem problem = DirectiveProblem::Unsupported;
    std::string name; ///< "else", "elif", "ifdef", "ifndef", "define", "pragma", "endif".
};

/**
 * @brief Everything one pass over a document's directives has to say about it.
 */
struct PreprocessorScan
{
    std::vector<ExcludedLineRange> excluded;
    std::vector<UnsupportedDirective> unsupported;
};

/**
 * @brief One pass yielding both the excluded ranges and the directives that will not compile.
 *        The two cannot be computed independently: whether a `#else` is an error depends on whether
 * @param[in] reportPragma Whether a surviving `#pragma` counts as unsupported. A stock host rejects
 *        every one - with no callback registered the add-on substitutes a failure and fails the
 */
PreprocessorScan ScanPreprocessor(std::string_view sourceCode,
                                  const ankerl::unordered_dense::set<std::string>& definedWords = {},
                                  const PreprocessorFeatures& features = {}, bool reportPragma = false);

/**
 * @brief True when a line falls inside any excluded range.
 * @note Linear in the number of ranges, which is the count of `#if` directives in one file -
 *        small enough that an index would cost more than it saves.
 */
bool IsLineExcluded(const std::vector<ExcludedLineRange>& ranges, uint32_t line);

/**
 * @brief Blanks out characters in excluded line ranges with spaces, preserving newlines (\\r and \\n).
 *        Replaces non-newline characters in lines identified by `ranges` with spaces (' ').
 * @param[in] sourceCode Document text.
 * @param[in] ranges Non-overlapping line intervals to blank (inclusive startLine and endLine).
 * @return Source code with excluded lines blanked by spaces.
 */
std::string BlankExcludedLines(std::string_view sourceCode, const std::vector<ExcludedLineRange>& ranges);

/**
 * @brief Words a predefined stub declares with `#define <word>`, in source order.
 *        `#define` is *not* an AngelScript directive and this does not make it one: written in a `.as`
 * @param[in] sourceCode Text of a predefined stub.
 * @return The defined words, with duplicates left in - the caller is inserting into a set.
 */
std::vector<std::string> ScanDefinedWords(std::string_view sourceCode);

/**
 * @brief Tracks an open preprocessor conditional block (#if/#ifdef/#ifndef) and its nesting depth.
 */
struct OpenDirective
{
    uint32_t branchStart = 0;  ///< Line of the directive that opened the current branch.
    bool excluded = false;     ///< True when the current branch is the dropped one.
    bool takenAlready = false; ///< True once some branch of this `#if` has been live.
};

/**
 * @brief Discovered preprocessor directive position and directive kind.
 */
struct DirectiveHit
{
    uint32_t line = 0;
    uint32_t startColumn = 0; ///< Byte column of the `#`.
    uint32_t endColumn = 0;   ///< One past the last character of the name.
    std::string_view name;
    std::string_view argument;
    bool touchesHash = true;
    char firstArgChar = 0;
};

/**
 * @brief State tracker for line numbers, byte offsets, and active directive blocks during region scanning.
 */
struct LineScanState
{
    size_t index{0};
    uint32_t currentLine{0};
    size_t lineStart{0};
    bool atLineStart{true};
};

/**
 * @brief Immutable input context and accumulators for preprocessor region scanning.
 */
struct PreprocessorScanContext
{
    PreprocessorScan& scan;
    std::vector<OpenDirective> stack;
    int excludedNesting{0};
    ankerl::unordered_dense::set<std::string> localWords;
    const ankerl::unordered_dense::set<std::string>& definedWords;
    const PreprocessorFeatures& features;
    bool usingLocalWords{false};
    bool sawMalformedIf{false};
    bool reportPragma{false};

    bool IsDefined(std::string_view word) const
    {
        const std::string key(word);
        return usingLocalWords ? localWords.contains(key) : definedWords.contains(key);
    }

    void CloseBranch(OpenDirective& open, uint32_t boundaryLine)
    {
        if (open.excluded)
        {
            scan.excluded.push_back(ExcludedLineRange{open.branchStart, boundaryLine});
        }
        else
        {
            open.takenAlready = true;
        }
    }

    void Report(const DirectiveHit& hit, DirectiveProblem problem)
    {
        scan.unsupported.push_back(
            UnsupportedDirective{hit.line, hit.startColumn, hit.endColumn, problem, std::string(hit.name)});
    }
};
} // namespace angel_lsp::utils
