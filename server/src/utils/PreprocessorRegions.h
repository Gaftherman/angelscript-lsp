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
     * @brief Lines removed by `#if` / `#endif`, mirroring CScriptBuilder's preprocessor.
     *
     * AngelScript has no preprocessor of its own; `#if` is handled by the CScriptBuilder add-on,
     * and its model is deliberately tiny (scriptbuilder.cpp, LoadScriptSection/ExcludeCode):
     *
     *  - Only `#if <identifier>` and `#endif` exist. There is no `#else`, no `#elif`, no `#ifdef`
     *    and no `#define` - words are defined by the *host application* calling DefineWord().
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
     * @return Excluded ranges in source order, non-overlapping.
     */
    std::vector<ExcludedLineRange> FindExcludedLineRanges(
        std::string_view sourceCode,
        const ankerl::unordered_dense::set<std::string> &definedWords = {});

    /**
     * @brief True when a line falls inside any excluded range.
     * @note Linear in the number of ranges, which is the count of `#if` directives in one file -
     *       small enough that an index would cost more than it saves.
     */
    bool IsLineExcluded(const std::vector<ExcludedLineRange> &ranges, uint32_t line);
}
