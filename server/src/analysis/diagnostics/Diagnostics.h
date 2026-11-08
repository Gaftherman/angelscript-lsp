#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Diagnostic severity level according to the LSP specification.
 */
enum class DiagnosticSeverity
{
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4
};

/**
 * @brief Position in a text document (0-indexed line and character).
 */
struct DiagnosticPosition
{
    uint32_t line = 0;
    uint32_t character = 0;
};

/**
 * @brief Range in a text document.
 */
struct DiagnosticRange
{
    DiagnosticPosition start;
    DiagnosticPosition end;
};

/**
 * @brief Related information location for an LSP Diagnostic.
 */
struct DiagnosticRelatedInformation
{
    DiagnosticRange range;
    std::string message;
    std::string fileUri;
};

/**
 * @brief Single LSP Diagnostic message published to the client.
 */
struct Diagnostic
{
    DiagnosticRange range;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string_view code;
    std::string_view source = "AngelScript";
    std::string message;
    std::string fileUri;
    std::vector<DiagnosticRelatedInformation> relatedInformation;
};
} // namespace angel_lsp::analysis
