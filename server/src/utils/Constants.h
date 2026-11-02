#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace angel_lsp::constants
{
/**
 * @brief Default diagnostics source identifier for LSP published diagnostics.
 */
inline constexpr std::string_view k_diagnosticSource = "AngelScript";

/**
 * @brief Sentinel type name used to represent poisoned or unresolvable expressions.
 */
inline constexpr std::string_view k_poisonTypeSentinel = "<poison>";

/**
 * @brief Sentinel type name used to represent an initializer list expression.
 */
inline constexpr std::string_view k_initializerListTypeSentinel = "<init_list>";

/**
 * @brief Default file extension for predefined API stub files.
 */
inline constexpr std::string_view k_defaultPredefinedExt = ".as.predefined";

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
} // namespace utf8

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
} // namespace scoring

/**
 * @brief Default limits and window thresholds.
 */
namespace limits
{
inline constexpr size_t DefaultWorkspaceSymbolResults = 100;
inline constexpr size_t MaxIndentationLookbackLines = 20;

/**
 * @brief Largest document this server will analyse, in bytes.
 *        A language server's input is whatever file the user opens, and analysis is superlinear in
 */
inline constexpr size_t MaxAnalysedDocumentBytes = 8u * 1024u * 1024u;
} // namespace limits
} // namespace angel_lsp::constants
