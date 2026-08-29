#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
    /**
     * @brief The shape of initializer list a type accepts, as its list factory declares it.
     *
     * AngelScript spells this out in the behaviour registration itself. The two standard add-ons
     * register:
     *
     *     asBEHAVE_LIST_FACTORY, "array<T>@ f(int&in type, int&in list) {repeat T}"
     *     asBEHAVE_LIST_FACTORY, "dictionary @f(int &in) {repeat {string, ?}}"
     *
     * The trailing `{...}` is the pattern, and it is the only thing that distinguishes a type that
     * takes `{1, 2, 3}` from one that takes `{{"a", 1}}` from one that takes no list at all. It is
     * not derivable from anything else: `optional<T>` is declared with the same single type
     * parameter as `array<T>` and registers no list factory, so the real compiler answers
     * `optional<int> o = {1};` with "Initialization lists cannot be used with 'optional<int>'".
     *
     * A predefined stub carries it as a `@listpattern` doc-comment tag, copied from the
     * registration:
     *
     *     /// @listpattern {repeat T}
     *     class array<T> { ... }
     *
     * A tag rather than a declaration because the pattern is not AngelScript source: written into a
     * class body, `{repeat T}` parses as a statement block containing a syntax error, which would
     * put red squiggles through the user's own stub.
     */
    struct ListPatternNode
    {
        enum class Kind
        {
            Group,   ///< `{ ... }` - a nested list is expected here.
            Repeat,  ///< `repeat X` - X, as many times as the list supplies.
            Type     ///< A type name, a template parameter, or `?` for any type.
        };

        Kind kind = Kind::Type;

        /** @brief For Kind::Type: the type named, `T` for a template parameter, `?` for any. */
        std::string typeName;

        /** @brief Group members in order, or the single repeated item for Kind::Repeat. */
        std::vector<ListPatternNode> children;
    };

    /** @brief A parsed pattern, plus whether it parsed at all. */
    struct ListPattern
    {
        ListPatternNode root;      ///< Always Kind::Group when valid.
        bool valid = false;

        /** @brief True for `?`, which AngelScript uses to mean "any type". */
        static bool IsAnyType(std::string_view typeName) { return typeName == "?"; }
    };

    /**
     * @brief Parses a list pattern such as `{repeat T}` or `{repeat {string, ?}}`.
     * @param text The pattern including its outer braces. Whitespace is insignificant.
     * @return The parsed pattern, or one with `valid == false` when the text is not a pattern.
     * @note An unparseable pattern is not an error to report. It means this server did not
     *       understand a stub it was handed, and the rules that consult it then stay silent -
     *       which is the same answer they give for a type that declares no pattern at all.
     */
    ListPattern ParseListPattern(std::string_view text);

    /**
     * @brief Extracts the `@listpattern` tag from a declaration's doc comment, if it has one.
     * @param sourceCode The whole document.
     * @param declStartLine Zero-based line the declaration starts on.
     * @return The pattern text including braces, or empty when the tag is absent.
     */
    std::string FindListPatternTag(const std::string &sourceCode, uint32_t declStartLine);
}
