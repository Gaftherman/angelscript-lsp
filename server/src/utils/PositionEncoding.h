#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::utils
{
    /**
     * @brief How an LSP client counts the `character` field of a `Position`.
     *
     * Tree-sitter always reports columns in bytes, so every position crossing the LSP
     * boundary has to be translated unless the client agreed to speak UTF-8. The kind in
     * use is negotiated once in `initialize` (see Server::HandleRequestsInitialized) and
     * threaded through every conversion below; UTF-16 is the protocol default and the only
     * value a server may assume when the client says nothing.
     */
    enum class PositionEncoding
    {
        Utf8,   ///< `character` counts bytes - identical to Tree-sitter's columns, conversions are identity.
        Utf16,  ///< `character` counts UTF-16 code units - the LSP default.
    };

    /**
     * @brief Precomputed byte offset of every line start in a document.
     *
     * LineStartOffset() and GetLine() scan from byte 0 on every call. That is fine once, and
     * quadratic when it happens per token: PositionCodec::EncodeSemanticTokens asks for a line per
     * token, so a 5000-line file with 20k tokens rescanned the whole document 20k times on every
     * semanticTokens request - and every delta request too. It short-circuits only for UTF-8
     * clients, and UTF-16 is the protocol default, so the slow path was the normal one.
     *
     * Build once per document revision and look up in O(1). Cheap to construct: one linear pass.
     */
    struct LineIndex
    {
        /** @brief Byte offset of the first character of each line. Always starts with 0. */
        std::vector<size_t> starts;

        /** @brief Indexes every line start in text. One O(n) pass. */
        static LineIndex Build(std::string_view text);

        /** @brief Number of lines, counting a trailing empty line after a final newline. */
        size_t LineCount() const noexcept { return starts.size(); }

        /** @brief Byte offset where a 0-indexed line starts, or text.size() when past the end. */
        size_t StartOffset(std::string_view text, uint32_t line) const noexcept;

        /** @brief The line's content without its terminator - GetLine() in O(1). */
        std::string_view Line(std::string_view text, uint32_t line) const noexcept;
    };

    /**
     * @brief Byte offset at which the given 0-indexed line starts.
     * @param text Full document text (UTF-8).
     * @param line 0-indexed line number.
     * @return Offset of the first byte of the line, or text.size() if the line is past the end.
     */
    size_t LineStartOffset(std::string_view text, uint32_t line);

    /**
     * @brief Extracts the content of a 0-indexed line, excluding its terminator.
     * @param text Full document text (UTF-8).
     * @param line 0-indexed line number.
     * @return View into text spanning the line without a trailing "\r\n" or "\n", empty if past the end.
     */
    std::string_view GetLine(std::string_view text, uint32_t line);

    /**
     * @brief Converts an LSP `Position.character` into a byte column within the same line.
     *
     * Characters past the end of the line clamp to the line length, as the spec requires.
     * A character landing inside a multi-byte sequence (or between the halves of a
     * surrogate pair) resolves to the start of the following character rather than to an
     * offset that would split it.
     *
     * @param line The line's content, without its terminator (see GetLine).
     * @param lspChar Character offset as sent by the client, in the negotiated encoding.
     * @param enc The negotiated encoding.
     * @return Byte offset from the start of the line.
     */
    uint32_t LspCharToByteColumn(std::string_view line, uint32_t lspChar, PositionEncoding enc);

    /**
     * @brief Converts a byte column (as produced by Tree-sitter) into an LSP `Position.character`.
     * @param line The line's content, without its terminator (see GetLine).
     * @param byteColumn Byte offset from the start of the line.
     * @param enc The negotiated encoding.
     * @return Character offset in the negotiated encoding.
     */
    uint32_t ByteToLspCharColumn(std::string_view line, uint32_t byteColumn, PositionEncoding enc);
}
