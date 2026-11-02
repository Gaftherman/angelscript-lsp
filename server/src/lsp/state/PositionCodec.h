#pragma once

#include "utils/PositionEncoding.h"

#include <lsp/types.h>

#include <string_view>
#include <vector>

namespace angel_lsp::codec
{
// Translates positions between client encoding (UTF-16/UTF-8) and Tree-sitter byte columns.

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
void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Position& position);

/**
 * @brief Converts an outbound byte-column range into the client's encoding, in place.
 */
void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Range& range);

/**
 * @brief Converts an outbound byte-column position into the client's encoding using a precomputed LineIndex.
 */
void Encode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc,
            lsp::Position& position);

/**
 * @brief Converts an outbound byte-column range into the client's encoding using a precomputed LineIndex.
 */
void Encode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc, lsp::Range& range);

/**
 * @brief Re-encodes a semantic tokens payload in place.
 *
 * The payload is a flat run of 5-tuples (deltaLine, deltaStart, length, type, modifiers) where
 * both `deltaStart` and `length` are measured in the negotiated encoding - so a document with
 * one accented character shifts every token after it on that line. Handled separately from
 * Encode() because the values have to be decoded to absolute positions, converted, and then
 * re-delta-encoded.
 */
void EncodeSemanticTokens(std::string_view text, utils::PositionEncoding enc, std::vector<lsp::uint>& data);
} // namespace angel_lsp::codec
