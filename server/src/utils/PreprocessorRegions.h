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
     *
     * Inclusive on both ends, and covering the `#if` and `#endif` lines themselves - CScriptBuilder
     * blanks those out too.
     */
    struct ExcludedLineRange
    {
        uint32_t startLine = 0;
        uint32_t endLine = 0;
    };

    /**
     * @brief Preprocessor extensions a host has added to CScriptBuilder, all off by default.
     *
     * None of these exist in the stock add-on - each was measured against the real compiler and
     * each is a syntax error or a swallowed line there, which is why every one of them defaults to
     * false and why the defaults reproduce today's behaviour byte for byte.
     *
     * They exist because patching scriptbuilder.cpp is common: it is a sample add-on shipped in the
     * SDK's source tree, hosts copy it into their own tree, and `#else` is the first thing they add.
     * An analyzer that cannot be told about that has to choose between reporting nothing inside
     * every conditional or reporting errors on code the compiler never sees, and both are wrong.
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
     * @brief Lines removed by `#if` / `#endif`, mirroring CScriptBuilder's preprocessor.
     *
     * AngelScript has no preprocessor of its own; `#if` is handled by the CScriptBuilder add-on,
     * and its model is deliberately tiny (scriptbuilder.cpp, LoadScriptSection/ExcludeCode):
     *
     *  - Only `#if <identifier>` and `#endif` exist. There is no `#else`, no `#elif`, no `#ifdef`
     *    and no `#define` - words are defined by the *host application* calling DefineWord().
     *
     *    Measured, not read off the source: `#if FOO / void a(){} / #else / void b(){} / #endif`
     *    followed by a call to `b()` is rejected with "No matching symbol 'b'", not with a
     *    complaint about `#else`. The `#else` branch did not become the taken one - it was blanked
     *    along with the rest of the block, because `#else` is not a directive and the exclusion
     *    runs to the `#endif` regardless. `#define`, `#ifdef` and a live `#else` are each a plain
     *    syntax error, and a `#pragma` fails the section outright unless the host registered a
     *    pragma callback (scriptbuilder.cpp:533).
     *  - If the identifier is not a defined word, everything up to and including the matching
     *    `#endif` is blanked out. Nesting is tracked, so an inner `#if` inside an excluded block
     *    does not end it early.
     *  - If it *is* defined, the directives are stripped and the body is compiled normally.
     *  - Blanking preserves newlines, so line numbers never shift.
     *
     * Analysing text the compiler never sees produces diagnostics about code that does not exist.
     * That was found by running this analyzer against the real compiler: AS-Harness's json.as keeps
     * a block of deliberately-unbuildable code inside `#if FALSE`, and every diagnostic we reported
     * for that file came from there.
     *
     * @param sourceCode Document text.
     * @param definedWords Words the host has defined. Empty - the default - means every `#if` block
     *        is excluded, which is exactly what an unconfigured CScriptBuilder does.
     * @param features Extensions the host added to its copy of the add-on. All off by default, in
     *        which case only `#if` and `#endif` are directives and everything between a dead `#if`
     *        and its `#endif` goes, `#else` included.
     * @return Excluded ranges in source order, non-overlapping.
     */
    std::vector<ExcludedLineRange> FindExcludedLineRanges(
        std::string_view sourceCode,
        const ankerl::unordered_dense::set<std::string> &definedWords = {},
        const PreprocessorFeatures &features = {});

    /**
     * @brief True when a line falls inside any excluded range.
     * @note Linear in the number of ranges, which is the count of `#if` directives in one file -
     *       small enough that an index would cost more than it saves.
     */
    bool IsLineExcluded(const std::vector<ExcludedLineRange> &ranges, uint32_t line);

    /**
     * @brief Words a predefined stub declares with `#define <word>`, in source order.
     *
     * `#define` is *not* an AngelScript directive and this does not make it one: written in a `.as`
     * the compiler rejects it outright, measured - CScriptBuilder leaves it in the source and the
     * tokenizer reports `Unexpected token`. What makes it meaningful in a `.as.predefined` is that
     * the stub is never compiled by AngelScript at all. It is this server's description of how the
     * host set its engine up, and DefineWord() is part of that setup, so `#define FOO` in a stub
     * says exactly one thing: the host calls `builder.DefineWord("FOO")`.
     *
     * Same walk as FindExcludedLineRanges, so a `#define` inside a comment or a string is not one.
     *
     * @param sourceCode Text of a predefined stub.
     * @return The defined words, with duplicates left in - the caller is inserting into a set.
     */
    std::vector<std::string> ScanDefinedWords(std::string_view sourceCode);
}
