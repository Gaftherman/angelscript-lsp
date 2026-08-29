#include "lsp/PositionCodec.h"

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

    void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Position &position)
    {
        if (enc == utils::PositionEncoding::Utf8)
            return;

        const std::string_view line = utils::GetLine(text, position.line);
        position.character = utils::ByteToLspCharColumn(line, position.character, enc);
    }

    void Encode(std::string_view text, utils::PositionEncoding enc, lsp::Range &range)
    {
        if (enc == utils::PositionEncoding::Utf8)
            return;

        Encode(text, enc, range.start);
        Encode(text, enc, range.end);
    }

    void EncodeSemanticTokens(std::string_view text, utils::PositionEncoding enc, std::vector<lsp::uint> &data)
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
}
