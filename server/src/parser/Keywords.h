#pragma once

#include <algorithm>
#include <array>
#include <string_view>

/**
 * @file
 * @brief The language's own words, in one place, split by what they are allowed to be.
 *
 * There were three of these lists - one in the analyzer, one in the formatter, one in completion -
 * of 53, 65 and 63 words. Twenty-four words appeared in some and not others, and every difference
 * was a behaviour difference nobody had chosen:
 *
 *   - the formatter did not colour `foreach` or `using`, because its list lacked them;
 *   - completion never suggested `and`, `or`, `not`, `xor` or `is`, the word operators;
 *   - the formatter coloured `with`, which is not an AngelScript keyword at all - it is
 *     JavaScript's, and does not appear anywhere in the grammar. The third thing in this codebase
 *     copied in from another language's tooling.
 *
 * The split below is not a matter of taste. It was measured, one file per word, against the real
 * compiler: `void t() { int <word>; }` and the same at global scope, for all seventy words the
 * three lists mentioned between them.
 */
namespace angel_lsp::parser::keywords
{
    /**
     * @brief Words that can never be an identifier. Rejected as a local and as a global alike.
     *
     * This is the set a diagnostic may be built on, and the only one. `string` is deliberately NOT
     * here even though `int string;` is rejected: `string` is a type the application registers, not
     * a word of the language, and a host that registers something else makes that name legal again.
     * The same reasoning keeps tests/parity/doc_p15 silent about a local shadowing a script class.
     */
    inline constexpr std::array<std::string_view, 53> k_reserved = {
        "and", "auto", "bool", "break", "case", "cast", "catch", "class", "const", "continue",
        "default", "do", "double", "else", "enum", "false", "float", "for", "foreach", "funcdef",
        "if", "import", "in", "inout", "int", "int8", "int16", "int32", "int64", "interface",
        "is", "mixin", "namespace", "not", "null", "or", "out", "private", "protected", "return",
        "switch", "true", "try", "typedef", "uint", "uint8", "uint16", "uint32", "uint64",
        "using", "void", "while", "xor",
    };

    /**
     * @brief Words the grammar knows, that are still legal as an identifier.
     *
     * Modifiers and contextual keywords: `int final;` compiles, and so does `int get;`. Measured,
     * all fifteen. They belong in colouring and in completion, and must never reach a rule that
     * rejects a name - which is exactly the mistake the split prevents.
     */
    inline constexpr std::array<std::string_view, 15> k_contextual = {
        "abstract", "delete", "explicit", "external", "final", "from", "function", "get",
        "override", "property", "public", "set", "shared", "super", "this",
    };

    /** @brief Reserved, so never a name. The analyzer's question. */
    [[nodiscard]] inline bool IsReserved(std::string_view word) noexcept
    {
        return std::find(k_reserved.begin(), k_reserved.end(), word) != k_reserved.end();
    }

    /** @brief A keyword of any kind - reserved or contextual. The formatter's question. */
    [[nodiscard]] inline bool IsKeyword(std::string_view word) noexcept
    {
        return IsReserved(word) ||
               std::find(k_contextual.begin(), k_contextual.end(), word) != k_contextual.end();
    }
}
