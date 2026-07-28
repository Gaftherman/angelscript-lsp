#pragma once

#include <string>
#include <cstdint>

namespace angel_lsp::analysis
{
    /** @brief Diagnostic severity level according to the LSP specification. */
    enum class DiagnosticSeverity
    {
        Error = 1,
        Warning = 2,
        Information = 3,
        Hint = 4
    };

    /** @brief Position in a text document (0-indexed line and character). */
    struct DiagnosticPosition
    {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    /** @brief Range in a text document. */
    struct DiagnosticRange
    {
        DiagnosticPosition start;
        DiagnosticPosition end;
    };

    /** @brief Represents a single LSP Diagnostic message to be published to the client. */
    struct Diagnostic
    {
        DiagnosticRange range;
        DiagnosticSeverity severity = DiagnosticSeverity::Error;
        std::string code;
        std::string source = "AngelScript";
        std::string message;
        std::string fileUri;
    };
}
