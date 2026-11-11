#pragma once

#include "analysis/diagnostics/Diagnostics.h"

#include <lsp/protocol.h>
#include <string>
#include <string_view>

namespace angel_lsp
{
/**
 * @brief Converts internal diagnostic severity to LSP protocol diagnostic severity.
 * @param[in] severity AngelScript internal diagnostic severity.
 * @return Protocol severity enum value.
 */
lsp::DiagnosticSeverity ToProtocolSeverity(analysis::DiagnosticSeverity severity);

/**
 * @brief Formats an LSP diagnostic message with an explicit severity and code prefix.
 * @param[in] severity AngelScript analysis severity.
 * @param[in] displayCode Formatted display code alias.
 * @param[in] rawMessage Original diagnostic message.
 * @return Prefixed message string.
 */
std::string FormatPrefixedDiagnosticMessage(analysis::DiagnosticSeverity severity,
                                            std::string_view displayCode, std::string_view rawMessage);

/**
 * @brief Resolves a VS Code settings URI for a diagnostic code so the code renders as a clickable link.
 * @param[in] code Canonical or alias diagnostic code.
 * @return Settings deep-link URI string or empty string when no code is present.
 */
std::string_view GetDiagnosticSettingsUri(std::string_view code);
} // namespace angel_lsp
