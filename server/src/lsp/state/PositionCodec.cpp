#include "lsp/state/PositionCodec.h"

namespace angel_lsp::codec
{
lsp::Position Decode(std::string_view text, utils::PositionEncoding enc, lsp::Position position)
{
    if (enc == utils::PositionEncoding::Utf8)
        return position;

    const std::string_view line = utils::GetLine(text, position.line);
    position.character = utils::LspCharToByteColumn(line, position.character, enc);
    return position;
}

lsp::Range Decode(std::string_view text, utils::PositionEncoding enc, lsp::Range range)
{
    if (enc == utils::PositionEncoding::Utf8)
        return range;

    range.start = Decode(text, enc, range.start);
    range.end = Decode(text, enc, range.end);
    return range;
}

void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Position& position)
{
    if (enc == utils::PositionEncoding::Utf8)
        return;

    const std::string_view line = utils::GetLine(text, position.line);
    position.character = utils::ByteToLspCharColumn(line, position.character, enc);
}

template <typename GetLine> static void EncodeRange(GetLine getLine, utils::PositionEncoding enc, lsp::Range& range)
{
    if (enc == utils::PositionEncoding::Utf8)
        return;

    const auto startLine = getLine(range.start.line);
    const auto endLine = range.end.line == range.start.line ? startLine : getLine(range.end.line);
    range.start.character = utils::ByteToLspCharColumn(startLine, range.start.character, enc);
    range.end.character = utils::ByteToLspCharColumn(endLine, range.end.character, enc);
}

void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Range& range)
{
    EncodeRange([&](lsp::uint line) { return utils::GetLine(text, line); }, enc, range);
}

void Encode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc,
            lsp::Position& position)
{
    if (enc == utils::PositionEncoding::Utf8)
        return;

    const std::string_view line = lineIndex.Line(text, position.line);
    position.character = utils::ByteToLspCharColumn(line, position.character, enc);
}

void Encode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc, lsp::Range& range)
{
    EncodeRange([&](lsp::uint line) { return lineIndex.Line(text, line); }, enc, range);
}

template <typename FindLine>
static bool TryEncodeRange(FindLine findLine, utils::PositionEncoding enc, lsp::Range& range)
{
    const auto hasValidColumn = [](std::string_view line, lsp::uint column)
    {
        return column <= line.size() &&
               (column == line.size() || (static_cast<unsigned char>(line[column]) & 0xC0u) != 0x80u);
    };
    const bool sameLine = range.start.line == range.end.line;
    if (range.end.line < range.start.line || (sameLine && range.end.character < range.start.character))
    {
        return false;
    }
    const auto startLine = findLine(range.start.line);
    if (!startLine || !hasValidColumn(*startLine, range.start.character))
    {
        return false;
    }
    const auto endLine = sameLine ? startLine : findLine(range.end.line);
    if (!endLine || !hasValidColumn(*endLine, range.end.character))
    {
        return false;
    }
    if (enc != utils::PositionEncoding::Utf8)
    {
        range.start.character = utils::ByteToLspCharColumn(*startLine, range.start.character, enc);
        range.end.character = utils::ByteToLspCharColumn(*endLine, range.end.character, enc);
    }
    return true;
}

bool TryEncode(const utils::LineIndex& lineIndex, std::string_view text, utils::PositionEncoding enc, lsp::Range& range)
{
    return TryEncodeRange(
        [&](lsp::uint line) -> std::optional<std::string_view>
        {
            if (line >= lineIndex.LineCount())
            {
                return std::nullopt;
            }
            return lineIndex.Line(text, line);
        },
        enc, range);
}

void DocumentPositionEncoder::Prepare(size_t count)
{
    constexpr size_t k_indexAfter = 4;
    if (m_index.LineCount() != 0)
    {
        return;
    }
    if (count > k_indexAfter - m_sparseLookups)
    {
        m_index = utils::LineIndex::Build(m_text);
    }
    else
    {
        m_sparseLookups += count;
    }
}

void DocumentPositionEncoder::Encode(lsp::Position& position) const
{
    if (m_index.LineCount() != 0)
    {
        codec::Encode(m_index, m_text, m_encoding, position);
    }
    else
    {
        codec::Encode(m_text, m_encoding, position);
    }
}

void DocumentPositionEncoder::Encode(lsp::Range& range) const
{
    if (m_index.LineCount() != 0)
    {
        codec::Encode(m_index, m_text, m_encoding, range);
    }
    else
    {
        codec::Encode(m_text, m_encoding, range);
    }
}

bool DocumentPositionEncoder::TryEncode(lsp::Range& range)
{
    if (m_index.LineCount() != 0)
    {
        return codec::TryEncode(m_index, m_text, m_encoding, range);
    }
    return TryEncodeRange(
        [&](lsp::uint line)
        {
            if (!m_cachedLineText || m_cachedLine != line)
            {
                m_cachedLine = line;
                m_cachedLineText = utils::FindLine(m_text, line);
            }
            return m_cachedLineText;
        },
        m_encoding, range);
}

void EncodeSemanticTokens(std::string_view text, utils::PositionEncoding enc, std::vector<lsp::uint>& data)
{
    constexpr size_t fields = 5;

    if (enc == utils::PositionEncoding::Utf8 || data.size() < fields)
        return;

    // Walk the run once, undoing the delta encoding just far enough to recover each token's
    // absolute (line, startColumn) in bytes, converting start and end, then re-emitting the
    // deltas in the client's encoding. Line numbers are encoding-independent, so deltaLine is
    // left alone and only the column baselines differ: prevByteStart drives the decode,
    // prevEncodedStart the re-encode, and past the first non-ASCII character on a line the two
    // no longer agree.
    // Built once for the whole run instead of rescanning the document from byte 0 for every
    // token, which is what utils::GetLine() does. With one GetLine() call per token this loop
    // was O(tokens x document size) - and it runs on every semanticTokens/full and every delta.
    const utils::LineIndex lineIndex = utils::LineIndex::Build(text);

    uint32_t prevLine = 0;
    uint32_t prevByteStart = 0;
    uint32_t prevEncodedStart = 0;

    for (size_t i = 0; i + fields <= data.size(); i += fields)
    {
        const uint32_t deltaLine = data[i];
        const uint32_t deltaStart = data[i + 1];
        const uint32_t length = data[i + 2];

        const uint32_t line = prevLine + deltaLine;
        const uint32_t byteStart = (deltaLine == 0) ? prevByteStart + deltaStart : deltaStart;

        const std::string_view lineText = lineIndex.Line(text, line);
        const uint32_t encodedStart = utils::ByteToLspCharColumn(lineText, byteStart, enc);
        const uint32_t encodedEnd = utils::ByteToLspCharColumn(lineText, byteStart + length, enc);

        data[i + 1] = (deltaLine == 0) ? encodedStart - prevEncodedStart : encodedStart;
        data[i + 2] = encodedEnd - encodedStart;

        prevLine = line;
        prevByteStart = byteStart;
        prevEncodedStart = encodedStart;
    }
}
} // namespace angel_lsp::codec
