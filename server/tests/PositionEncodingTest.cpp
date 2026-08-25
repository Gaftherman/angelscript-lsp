#include <doctest/doctest.h>

#include "utils/PositionEncoding.h"
#include "utils/Utils.h"
#include "lsp/PositionCodec.h"

using namespace angel_lsp::utils;

// =====================================================================================
// The whole point of this file: LSP counts Position::character in UTF-16 code units,
// Tree-sitter counts TSPoint::column in bytes, and the two only agree on pure-ASCII
// lines. Every fixture below therefore puts a non-ASCII character *before* the thing
// being located - that is the only arrangement in which the bug is observable.
//
// Byte budget for the characters used here:
//   'o'  -> 1 byte,  1 UTF-16 unit
//   'ó'  -> 2 bytes, 1 UTF-16 unit  (U+00F3)
//   '€'  -> 3 bytes, 1 UTF-16 unit  (U+20AC)
//   emoji -> 4 bytes, 2 UTF-16 units (U+1F52B, encoded as a surrogate pair)
// =====================================================================================

namespace
{
    // U+1F52B written out byte by byte so the fixture does not depend on the source file's
    // own encoding surviving every toolchain.
    constexpr const char *k_astral = "\xF0\x9F\x94\xAB";
}

// -------------------------------------------------------------------------------------
// LineStartOffset / GetLine
// -------------------------------------------------------------------------------------

TEST_CASE("LineStartOffset - returns the byte offset each line begins at")
{
    const std::string text = "alpha\nbeta\ngamma";
    CHECK(LineStartOffset(text, 0) == 0);
    CHECK(LineStartOffset(text, 1) == 6);
    CHECK(LineStartOffset(text, 2) == 11);
}

TEST_CASE("LineStartOffset - a line past the end clamps to the end of the text")
{
    const std::string text = "only one line";
    CHECK(LineStartOffset(text, 7) == text.size());
}

TEST_CASE("GetLine - excludes the line terminator, including a CRLF carriage return")
{
    const std::string unixText = "alpha\nbeta";
    CHECK(GetLine(unixText, 0) == "alpha");
    CHECK(GetLine(unixText, 1) == "beta");

    const std::string windowsText = "alpha\r\nbeta\r\n";
    CHECK(GetLine(windowsText, 0) == "alpha");
    CHECK(GetLine(windowsText, 1) == "beta");
}

TEST_CASE("GetLine - a line past the end is empty rather than out of range")
{
    CHECK(GetLine("alpha", 4).empty());
    CHECK(GetLine("", 0).empty());
}

// -------------------------------------------------------------------------------------
// LspCharToByteColumn - inbound conversion (client -> Tree-sitter)
// -------------------------------------------------------------------------------------

TEST_CASE("LspCharToByteColumn - UTF-8 encoding is an identity, clamped to the line length")
{
    const std::string_view line = "// \xC3\xB3 x";
    CHECK(LspCharToByteColumn(line, 4, PositionEncoding::Utf8) == 4);
    CHECK(LspCharToByteColumn(line, 999, PositionEncoding::Utf8) == line.size());
}

TEST_CASE("LspCharToByteColumn - a two-byte character shifts every column after it")
{
    // "// o-acute x": '/' '/' ' ' 'oacute'(2 bytes) ' ' 'x'
    // UTF-16 columns: 0   1   2    3               4   5
    // byte columns:   0   1   2    3               5   6
    const std::string_view line = "// \xC3\xB3 x";

    CHECK(LspCharToByteColumn(line, 3, PositionEncoding::Utf16) == 3); // start of the accent
    CHECK(LspCharToByteColumn(line, 4, PositionEncoding::Utf16) == 5); // the space after it
    CHECK(LspCharToByteColumn(line, 5, PositionEncoding::Utf16) == 6); // 'x'
}

TEST_CASE("LspCharToByteColumn - a three-byte character costs one UTF-16 unit")
{
    const std::string_view line = "\xE2\x82\xAC" "x";
    CHECK(LspCharToByteColumn(line, 1, PositionEncoding::Utf16) == 3);
}

TEST_CASE("LspCharToByteColumn - an astral character costs two UTF-16 units")
{
    const std::string line = std::string("a") + k_astral + "b";
    CHECK(LspCharToByteColumn(line, 1, PositionEncoding::Utf16) == 1); // start of the emoji
    CHECK(LspCharToByteColumn(line, 3, PositionEncoding::Utf16) == 5); // 'b', past both surrogates
}

TEST_CASE("LspCharToByteColumn - a column landing inside a surrogate pair does not split the character")
{
    // Column 2 is the low surrogate, which has no byte of its own. Resolving to the start of
    // the following character is the only answer that keeps the offset on a character boundary.
    const std::string line = std::string("a") + k_astral + "b";
    CHECK(LspCharToByteColumn(line, 2, PositionEncoding::Utf16) == 5);
}

TEST_CASE("LspCharToByteColumn - a column past the end of the line clamps to the line length")
{
    const std::string_view line = "// \xC3\xB3";
    CHECK(LspCharToByteColumn(line, 99, PositionEncoding::Utf16) == line.size());
}

// -------------------------------------------------------------------------------------
// ByteToLspCharColumn - outbound conversion (Tree-sitter -> client)
// -------------------------------------------------------------------------------------

TEST_CASE("ByteToLspCharColumn - UTF-8 encoding is an identity, clamped to the line length")
{
    const std::string_view line = "// \xC3\xB3 x";
    CHECK(ByteToLspCharColumn(line, 5, PositionEncoding::Utf8) == 5);
    CHECK(ByteToLspCharColumn(line, 999, PositionEncoding::Utf8) == line.size());
}

TEST_CASE("ByteToLspCharColumn - undoes the shift a multi-byte character introduces")
{
    const std::string_view line = "// \xC3\xB3 x";
    CHECK(ByteToLspCharColumn(line, 3, PositionEncoding::Utf16) == 3);
    CHECK(ByteToLspCharColumn(line, 5, PositionEncoding::Utf16) == 4);
    CHECK(ByteToLspCharColumn(line, 6, PositionEncoding::Utf16) == 5);
}

TEST_CASE("ByteToLspCharColumn - an astral character counts as two units")
{
    const std::string line = std::string("a") + k_astral + "b";
    CHECK(ByteToLspCharColumn(line, 5, PositionEncoding::Utf16) == 3);
}

TEST_CASE("Column conversions round-trip on every character boundary of a mixed-width line")
{
    const std::string line = std::string("int \xC3\xB3 = \xE2\x82\xAC;  // ") + k_astral;

    for (size_t byteColumn = 0; byteColumn <= line.size(); ++byteColumn)
    {
        // Only character boundaries round-trip: a byte in the middle of a sequence has no
        // UTF-16 column of its own, so skip continuation bytes.
        if (byteColumn < line.size() && (static_cast<unsigned char>(line[byteColumn]) & 0xC0) == 0x80)
            continue;

        const uint32_t asUtf16 = ByteToLspCharColumn(line, static_cast<uint32_t>(byteColumn), PositionEncoding::Utf16);
        CHECK(LspCharToByteColumn(line, asUtf16, PositionEncoding::Utf16) == byteColumn);
    }
}

// -------------------------------------------------------------------------------------
// PositionToOffset - the document-wide entry point
// -------------------------------------------------------------------------------------

TEST_CASE("PositionToOffset - resolves a column on a line containing accented text")
{
    // Line 0 is "// o-acute" (5 bytes), so line 1 starts at byte 6.
    const std::string text = "// \xC3\xB3\nint ammo = 5;\n";

    CHECK(PositionToOffset(text, 0, 4, PositionEncoding::Utf16) == 5);  // end of line 0
    CHECK(PositionToOffset(text, 1, 4, PositionEncoding::Utf16) == 10); // 'a' of "ammo"
    CHECK(text.substr(PositionToOffset(text, 1, 4, PositionEncoding::Utf16), 4) == "ammo");
}

TEST_CASE("PositionToOffset - a character past the end of a line clamps to that line, not the next")
{
    const std::string text = "// \xC3\xB3\nint ammo = 5;\n";
    CHECK(PositionToOffset(text, 0, 99, PositionEncoding::Utf16) == 5);
}

TEST_CASE("PositionToOffset - a line past the end clamps to the end of the text")
{
    const std::string text = "// \xC3\xB3\n";
    CHECK(PositionToOffset(text, 42, 0, PositionEncoding::Utf16) == text.size());
}

// -------------------------------------------------------------------------------------
// ApplyIncrementalChange - the destructive case: an edit landing mid-character splits a
// UTF-8 sequence, and from that point the server's buffer and the editor's disagree
// permanently.
// -------------------------------------------------------------------------------------

TEST_CASE("ApplyIncrementalChange - an insertion after an accented character lands on a character boundary")
{
    std::string buffer = "// \xC3\xB3\nint y = 2;\n";

    // The editor reports the caret just past the accent as UTF-16 column 4; byte column 4
    // would be the *second* byte of the two-byte sequence.
    ApplyIncrementalChange(buffer, 0, 4, 0, 4, "!", PositionEncoding::Utf16);

    CHECK(buffer == "// \xC3\xB3!\nint y = 2;\n");
}

TEST_CASE("ApplyIncrementalChange - a replacement spanning an accented character removes it whole")
{
    std::string buffer = "int \xC3\xB3 = 1;\n";

    ApplyIncrementalChange(buffer, 0, 4, 0, 5, "x", PositionEncoding::Utf16);

    CHECK(buffer == "int x = 1;\n");
}

TEST_CASE("ApplyIncrementalChange - a multi-line replacement across accented lines")
{
    std::string buffer = "// \xC3\xB3\n// \xE2\x82\xAC\nkeep\n";

    // End column 4 on line 1 is past the euro sign, so the replacement swallows both accented
    // characters; column 3 would stop at its first byte and leave it stranded.
    ApplyIncrementalChange(buffer, 0, 3, 1, 4, "X", PositionEncoding::Utf16);

    CHECK(buffer == "// X\nkeep\n");
}

TEST_CASE("ApplyIncrementalChange - UTF-8 clients are unaffected, byte columns pass straight through")
{
    std::string buffer = "// \xC3\xB3\nint y = 2;\n";

    ApplyIncrementalChange(buffer, 0, 5, 0, 5, "!", PositionEncoding::Utf8);

    CHECK(buffer == "// \xC3\xB3!\nint y = 2;\n");
}

// -------------------------------------------------------------------------------------
// codec - the LSP result shapes
// -------------------------------------------------------------------------------------

TEST_CASE("codec::Decode - converts an inbound position into byte columns")
{
    const std::string text = "// \xC3\xB3 x\n";

    lsp::Position position;
    position.line = 0;
    position.character = 5;

    const lsp::Position decoded = angel_lsp::codec::Decode(text, PositionEncoding::Utf16, position);
    CHECK(decoded.line == 0);
    CHECK(decoded.character == 6);
}

TEST_CASE("codec::Encode - converts an outbound range back into the client's encoding")
{
    const std::string text = "// \xC3\xB3 x\n";

    lsp::Range range;
    range.start.line = 0;
    range.start.character = 6; // byte column of 'x'
    range.end.line = 0;
    range.end.character = 7;

    angel_lsp::codec::Encode(text, PositionEncoding::Utf16, range);

    CHECK(range.start.character == 5);
    CHECK(range.end.character == 6);
}

TEST_CASE("codec::EncodeSemanticTokens - re-encodes both the column deltas and the token lengths")
{
    // "ab <o-acute> cd": 'a'(0) 'b'(1) ' '(2) accent(3..4) ' '(5) 'c'(6) 'd'(7)
    // byte columns:   ab -> 0, cd -> 6
    // UTF-16 columns: ab -> 0, cd -> 5
    const std::string text = "ab \xC3\xB3 cd";

    std::vector<lsp::uint> data = {
        0, 0, 2, 0, 0, // "ab" at byte column 0, length 2
        0, 6, 2, 0, 0, // "cd", delta 6 from the previous token's byte column
    };

    angel_lsp::codec::EncodeSemanticTokens(text, PositionEncoding::Utf16, data);

    CHECK(data[1] == 0);
    CHECK(data[2] == 2);
    CHECK(data[6] == 5); // one byte narrower, because the accent costs 2 bytes but 1 unit
    CHECK(data[7] == 2);
}

TEST_CASE("codec::EncodeSemanticTokens - a token length spanning an accented character shrinks")
{
    // "s<o-acute>z" is one 4-byte, 3-unit identifier starting at byte column 0.
    const std::string text = "s\xC3\xB3z = 1;";

    std::vector<lsp::uint> data = { 0, 0, 4, 0, 0 };

    angel_lsp::codec::EncodeSemanticTokens(text, PositionEncoding::Utf16, data);

    CHECK(data[1] == 0);
    CHECK(data[2] == 3);
}

TEST_CASE("codec::EncodeSemanticTokens - tokens on later lines keep their line deltas")
{
    const std::string text = "// \xC3\xB3\nint x;\n";

    std::vector<lsp::uint> data = {
        1, 4, 1, 0, 0, // 'x' on line 1, byte column 4 - an ASCII line, so nothing moves
    };

    angel_lsp::codec::EncodeSemanticTokens(text, PositionEncoding::Utf16, data);

    CHECK(data[0] == 1);
    CHECK(data[1] == 4);
    CHECK(data[2] == 1);
}

TEST_CASE("codec::EncodeSemanticTokens - UTF-8 clients get the payload back untouched")
{
    const std::string text = "ab \xC3\xB3 cd";

    std::vector<lsp::uint> data = { 0, 0, 2, 0, 0, 0, 6, 2, 0, 0 };
    const std::vector<lsp::uint> original = data;

    angel_lsp::codec::EncodeSemanticTokens(text, PositionEncoding::Utf8, data);

    CHECK(data == original);
}

TEST_CASE("codec::Encode - WorkspaceEdit text edits with UTF-16 non-ASCII characters")
{
    const std::string text = "// \xC3\xB3 x";
    lsp::Range range;
    range.start.line = 0;
    range.start.character = 5; // byte column of 'x'
    range.end.line = 0;
    range.end.character = 6;

    angel_lsp::codec::Encode(text, PositionEncoding::Utf16, range);

    CHECK(range.start.character == 4);
    CHECK(range.end.character == 5);
}

