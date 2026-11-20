#pragma once

#include "utils/PositionEncoding.h"

#include <cstddef>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::codec
{
// Translates positions between the negotiated client encoding and parser byte columns.

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
 * @brief Encodes a range whose endpoints are valid byte columns in the supplied source.
 * @param[in] lineIndex Line index for the source snapshot.
 * @param[in] text Source snapshot that owns the indexed lines.
 * @param[in] enc Client position encoding.
 * @param[in,out] range Byte range to convert.
 * @return True if converted, or false if an endpoint is invalid and the range remains unchanged.
 */
[[nodiscard]] bool TryEncode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc,
                             lsp::Range& range);

/**
 * @brief Reuses line lookups across outbound positions and ranges in one document snapshot.
 * @note The source text must outlive the encoder.
 */
class DocumentPositionEncoder
{
  public:
    /**
     * @brief Borrows the source text for an outbound batch.
     * @param[in] text Document snapshot in UTF-8.
     * @param[in] encoding Negotiated client encoding.
     */
    DocumentPositionEncoder(std::string_view text, utils::PositionEncoding encoding)
        : m_text(text), m_encoding(encoding)
    {
    }

    /**
     * @brief Accounts for upcoming lookups and indexes the document when the batch warrants it.
     * @param[in] count Number of additional positions or ranges that need line lookups.
     */
    void Prepare(size_t count);

    /**
     * @brief Converts a byte-column position to the client encoding.
     * @param[in,out] position Document position to convert.
     */
    void Encode(lsp::Position& position) const;

    /**
     * @brief Converts a byte-column range to the client encoding.
     * @param[in,out] range Document range to convert.
     */
    void Encode(lsp::Range& range) const;

    /**
     * @brief Validates a byte range and converts it to the client encoding.
     * @param[in,out] range Document range to validate and convert.
     * @return True on success. An invalid range remains unchanged.
     */
    [[nodiscard]] bool TryEncode(lsp::Range& range);

  private:
    std::string_view m_text;
    utils::PositionEncoding m_encoding;
    utils::LineIndex m_index;
    size_t m_sparseLookups = 0;
    lsp::uint m_cachedLine = 0;
    std::optional<std::string_view> m_cachedLineText;
};

/**
 * @brief Re-encodes a semantic tokens payload in place.
 *        The payload is a flat run of 5-tuples (deltaLine, deltaStart, length, type, modifiers) where
 */
void EncodeSemanticTokens(std::string_view text, utils::PositionEncoding enc, std::vector<lsp::uint>& data);
} // namespace angel_lsp::codec
