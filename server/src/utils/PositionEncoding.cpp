#include "utils/PositionEncoding.h"
#include "utils/Constants.h"

#include <algorithm>

namespace angel_lsp::utils
{
    namespace
    {
        /**
         * @brief Bytes consumed and UTF-16 code units produced by one UTF-8 sequence.
         */
        struct Utf8Step
        {
            uint32_t bytes = 1;
            uint32_t units = 1;
        };

        /**
         * @brief Classifies a UTF-8 lead byte.
         *
         * A byte that is not a valid lead (a stray continuation byte, or 0xF8+) is reported as a
         * one-byte, one-unit character so that malformed input still advances instead of looping
         * forever - the columns it produces are meaningless, but so was the input.
         */
        Utf8Step StepAt(unsigned char lead)
        {
            using namespace angel_lsp::constants::utf8;
            if (lead < AsciiMask)
                return {1, 1};
            if ((lead & TwoByteMask) == TwoByteExpected)
                return {2, 1};
            if ((lead & ThreeByteMask) == ThreeByteExpected)
                return {3, 1};
            if ((lead & FourByteMask) == FourByteExpected)
                return {4, 2}; // Astral plane: one codepoint, two UTF-16 surrogates.
            return {1, 1};
        }
    }

    size_t LineStartOffset(std::string_view text, uint32_t line)
    {
        size_t offset = 0;
        uint32_t currentLine = 0;

        while (offset < text.size() && currentLine < line)
        {
            if (text[offset] == '\n')
                currentLine++;
            offset++;
        }

        return offset;
    }

    std::string_view GetLine(std::string_view text, uint32_t line)
    {
        size_t start = LineStartOffset(text, line);
        if (start >= text.size())
            return {};

        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();

        if (end > start && text[end - 1] == '\r')
            end--;

        return text.substr(start, end - start);
    }

    uint32_t LspCharToByteColumn(std::string_view line, uint32_t lspChar, PositionEncoding enc)
    {
        const uint32_t lineBytes = static_cast<uint32_t>(line.size());

        if (enc == PositionEncoding::Utf8)
            return std::min(lspChar, lineBytes);

        uint32_t byteIndex = 0;
        uint32_t units = 0;

        while (byteIndex < lineBytes && units < lspChar)
        {
            const Utf8Step step = StepAt(static_cast<unsigned char>(line[byteIndex]));
            if (byteIndex + step.bytes > lineBytes)
                break; // Truncated sequence at end of line - stop rather than read past it.

            byteIndex += step.bytes;
            units += step.units;
        }

        return byteIndex;
    }

    uint32_t ByteToLspCharColumn(std::string_view line, uint32_t byteColumn, PositionEncoding enc)
    {
        const uint32_t lineBytes = static_cast<uint32_t>(line.size());

        if (enc == PositionEncoding::Utf8)
            return std::min(byteColumn, lineBytes);

        const uint32_t limit = std::min(byteColumn, lineBytes);
        uint32_t byteIndex = 0;
        uint32_t units = 0;

        while (byteIndex < limit)
        {
            const Utf8Step step = StepAt(static_cast<unsigned char>(line[byteIndex]));
            if (byteIndex + step.bytes > lineBytes)
                break;

            byteIndex += step.bytes;
            units += step.units;
        }

        return units;
    }
}
