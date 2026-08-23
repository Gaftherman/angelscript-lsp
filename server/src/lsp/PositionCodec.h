#pragma once

#include "utils/PositionEncoding.h"

#include <lsp/types.h>

#include <string_view>
#include <vector>

namespace angel_lsp::codec
{
    /**
     * @brief Translates positions between the encoding a client speaks and the byte columns
     *        Tree-sitter reports.
     *
     * Every feature handler works in Tree-sitter's own coordinates: `TSPoint::column` is a byte
     * offset into the line. `lsp::Position::character` is whatever the client and server agreed on
     * in `initialize` - UTF-16 code units unless the client offered UTF-8. On a pure-ASCII line the
     * two coincide, which is why this only ever surfaces on documents containing accented text,
     * emoji, or any other non-ASCII byte earlier in the line.
     *
     * All functions short-circuit to a no-op when `enc` is Utf8, so the negotiated-UTF-8 path costs
     * nothing beyond the branch.
     */

    /**
     * @brief Converts an inbound client position into Tree-sitter byte columns.
     * @param text Full text of the document the position refers to.
     */
    lsp::Position Decode(std::string_view text, utils::PositionEncoding enc, lsp::Position position);

    /**
     * @brief Converts an inbound client range into Tree-sitter byte columns.
     */
    lsp::Range Decode(std::string_view text, utils::PositionEncoding enc, lsp::Range range);

    /**
     * @brief Converts an outbound byte-column position into the client's encoding, in place.
     */
    void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Position &position);

    /**
     * @brief Converts an outbound byte-column range into the client's encoding, in place.
     */
    void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Range &range);

    /**
     * @brief Re-encodes a semantic tokens payload in place.
     *
     * The payload is a flat run of 5-tuples (deltaLine, deltaStart, length, type, modifiers) where
     * both `deltaStart` and `length` are measured in the negotiated encoding - so a document with
     * one accented character shifts every token after it on that line. Handled separately from
     * Encode() because the values have to be decoded to absolute positions, converted, and then
     * re-delta-encoded.
     */
    void EncodeSemanticTokens(std::string_view text, utils::PositionEncoding enc, std::vector<lsp::uint> &data);
}
