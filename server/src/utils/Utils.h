#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <spdlog/fmt/fmt.h>
#include <string>

#include "utils/PositionEncoding.h"

#include <vector>

namespace angel_lsp::utils
{
std::string UriToPath(const std::string& uri);
std::string PathToUri(const std::string& filePath);
/**
 * @brief Decodes percent-encoded characters in a URI or URI component (e.g. %3A%3A -> ::, %20 -> space).
 * @param in Input string containing percent-encoded sequences.
 * @return Decoded string.
 */
std::string UrlDecode(std::string_view in);

/**
 * @brief Compares two text buffers character-by-character ignoring carriage return ('\\r') differences.
 * @param[in] a First text buffer.
 * @param[in] b Second text buffer.
 * @return True if both buffers match identically excluding carriage returns.
 */
bool TextContentMatchesIgnoringLineEndings(std::string_view a, std::string_view b) noexcept;
/**
 * @brief Converts an LSP position into a byte offset into the document text.
 * @param text Full document text (UTF-8).
 * @param line 0-indexed line number.
 * @param character Character offset within the line, in the negotiated encoding.
 * @param enc The position encoding negotiated with the client.
 * @return Byte offset, clamped to the end of the line and then to the end of the text.
 */
size_t PositionToOffset(const std::string& text, uint32_t line, uint32_t character, PositionEncoding enc);

/**
 * @brief Represents a zero-indexed text range for an incremental LSP edit.
 */
struct TextChangeRange
{
    uint32_t startLine{0};
    uint32_t startCharacter{0};
    uint32_t endLine{0};
    uint32_t endCharacter{0};
};

/**
 * @brief Applies one LSP incremental content change to the document buffer in place.
 * @param[in,out] buffer Document text buffer to mutate in place.
 * @param[in] range Coordinate range of the replaced region.
 * @param[in] newText Replacement text to insert.
 * @param[in] enc The position encoding negotiated with the client - the range is expressed in it.
 */
void ApplyIncrementalChange(std::string& buffer, const TextChangeRange& range, const std::string& newText,
                            PositionEncoding enc);
/**
 * @brief Checks whether a file is a predefined stub describing the host application's API.
 *
 * Two spellings count. One is the configured suffix, which this project writes as
 * `<name>.as.predefined`. The other is a file named exactly `as.predefined`, which is
 * AngelScript's own convention and what the community's stubs are called - the extension
 * registers that filename as AngelScript in package.json, so the server recognising it too is
 * what makes the two halves agree.
 *
 * @param fileUri URI or path of the file.
 * @param extension Configured stub suffix, e.g. ".as.predefined". Empty disables suffix matching.
 * @return True if the file should be treated as a stub.
 */
bool IsPredefinedFile(const std::string_view& fileUri, const std::string_view extension);

/**
 * @brief Sanitizes predefined stub content before parsing by blanking inline list patterns.
 *
 * Native AngelScript stubs (.predefined files) can contain documentation notation for list factories:
 *     array(int &in type, int &in list) {repeat T};
 *     dictionary(int &in type, int &in list) {repeat {string, ?}};
 * Tree-Sitter grammar cannot parse `{repeat ...}` tokens inside class bodies and reports syntax errors,
 * which causes the AST parser to drop whole classes (e.g. dictionary).
 * Blanking the `{...}` pattern between `)` and `;` with spaces preserves exact line and column numbers
 * without shifting offsets, allowing Tree-Sitter to parse valid method declarations cleanly.
 *
 * @param source The stub's full text.
 * @return Sanitized text where inline patterns are replaced by spaces.
 */
std::string SanitizePredefinedContent(std::string_view source);

/**
 * @brief Glob match over a `/`-separated path.
 *
 * `?` matches one character, `*` matches within a single segment, and `**` spans any number of
 * segments including none - the same three the exclude settings every editor ships use,
 * so a user can paste what they already have.
 */
bool MatchesGlob(std::string_view path, std::string_view pattern);

/** @brief True when any pattern matches the path. */
bool IsExcludedPath(std::string_view path, const std::vector<std::string>& patterns);

/**
 * @brief True when a directory should not be descended into.
 *
 * Distinct from IsExcludedPath because a pattern like `**` + `/build/` + `**` names what is INSIDE `build` and never
 * `build` itself, and pruning is the whole point: filtering results afterwards still walks
 * every file under it. The trailing slash-star-star is dropped before the directory is tested.
 */
bool IsExcludedDirectory(std::string_view path, const std::vector<std::string>& patterns);

/**
 * @brief Checks whether the given type name is a standard AngelScript primitive type.
 * @param typeName The name of the type to check.
 * @return True if the type is a standard AngelScript primitive type; otherwise false.
 */
bool IsPrimitiveType(const std::string& typeName);

/**
 * @brief Checks whether a target path is strictly contained within a root directory.
 * @param[in] root Root directory path.
 * @param[in] target Target path to test for containment.
 * @return True if target is contained within root, false otherwise.
 */
bool IsWithinDirectory(const std::filesystem::path& root, const std::filesystem::path& target);

/**
 * @brief Advances index past an escaped string or character literal up to the matching quote or line break.
 * @param[in] sourceCode Document text buffer.
 * @param[in,out] index Scanner index positioned at the opening quote; advanced past closing quote or to line break.
 * @param[in] quote Quote delimiter ('"' or '\'').
 */
void SkipEscapedStringLiteral(std::string_view sourceCode, size_t& index, char quote) noexcept;

/**
 * @brief Dispatches comment skipping to line or block comment handlers based on the next character.
 * @tparam State Scanner state struct containing an `index` field.
 * @tparam LineCommentFn Functor or function pointer accepting (std::string_view, State&).
 * @tparam BlockCommentFn Functor or function pointer accepting (std::string_view, State&).
 * @param[in] sourceCode Document text buffer.
 * @param[in,out] state Scanner state.
 * @param[in] skipLine Line comment skipping handler.
 * @param[in] skipBlock Block comment skipping handler.
 * @return True if a comment was detected and consumed, false otherwise.
 */
template <typename State, typename LineCommentFn, typename BlockCommentFn>
inline bool TrySkipCommentDispatch(std::string_view sourceCode, State& state, LineCommentFn&& skipLine,
                                   BlockCommentFn&& skipBlock) noexcept
{
    if (state.index >= sourceCode.size() || sourceCode[state.index] != '/' || state.index + 1 >= sourceCode.size())
    {
        return false;
    }
    const char next = sourceCode[state.index + 1];
    if (next == '/')
    {
        skipLine(sourceCode, state);
        return true;
    }
    if (next == '*')
    {
        skipBlock(sourceCode, state);
        return true;
    }
    return false;
}

/**
 * @brief Compares two strings case-insensitively for equality.
 * @param[in] a First string to compare.
 * @param[in] b Second string to compare.
 * @return True if both strings match identically ignoring ASCII case.
 */
inline bool CaseInsensitiveEquals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Checks if a string starts with a given prefix case-insensitively.
 * @param[in] str Full string to check.
 * @param[in] prefix Prefix to check against.
 * @return True if str begins with prefix ignoring ASCII case.
 */
inline bool CaseInsensitiveStartsWith(std::string_view str, std::string_view prefix) noexcept
{
    if (str.size() < prefix.size())
    {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
            return false;
        }
    }
    return true;
}
} // namespace angel_lsp::utils