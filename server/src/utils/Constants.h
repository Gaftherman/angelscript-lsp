#pragma once

#include <cstdint>
#include <limits>

namespace angel_lsp::constants
{
    /**
     * @brief Sentinel value representing an invalid or unmatchable byte offset.
     */
    inline constexpr uint32_t InvalidByteOffset = std::numeric_limits<uint32_t>::max();

    /**
     * @brief UTF-8 lead byte classification bitmasks (RFC 3629).
     */
    namespace utf8
    {
        inline constexpr uint8_t AsciiMask = 0x80;
        inline constexpr uint8_t TwoByteMask = 0xE0;
        inline constexpr uint8_t TwoByteExpected = 0xC0;
        inline constexpr uint8_t ThreeByteMask = 0xF0;
        inline constexpr uint8_t ThreeByteExpected = 0xE0;
        inline constexpr uint8_t FourByteMask = 0xF8;
        inline constexpr uint8_t FourByteExpected = 0xF0;
    }

    /**
     * @brief Workspace symbol search heuristic match scores.
     */
    namespace scoring
    {
        inline constexpr int ExactMatch = 110;
        inline constexpr int ExactMatchCaseInsensitive = 100;
        inline constexpr int PrefixMatch = 85;
        inline constexpr int PrefixMatchCaseInsensitive = 80;
        inline constexpr int SubstringMatch = 50;
        inline constexpr int FuzzyMatch = 20;
    }

    /**
     * @brief Default limits and window thresholds.
     */
    namespace limits
    {
        inline constexpr size_t DefaultWorkspaceSymbolResults = 100;
        inline constexpr size_t MaxIndentationLookbackLines = 20;

        /**
         * @brief Largest document this server will analyse, in bytes.
         *
         * A language server's input is whatever file the user opens, and analysis is superlinear in
         * places, so an enormous file is a way to hang the session rather than merely slow it. Past
         * this the document is still tracked, synced and served for navigation - it simply is not
         * analysed, which degrades gracefully instead of freezing.
         *
         * 8 MB is far beyond any hand-written script: the largest file in the Sven Co-op corpus
         * this project tests against is under 200 KB.
         */
        inline constexpr size_t MaxAnalysedDocumentBytes = 8u * 1024u * 1024u;
    }
}
