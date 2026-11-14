#pragma once

#include "config/ServerConfig.h"

#include <istream>
#include <ostream>

namespace angel_lsp::mcp
{

/**
 * @brief Serves the MCP tools over newline-delimited JSON-RPC 2.0 until the input ends.
 * @note A malformed line gets a parse error and the session continues.
 * @note A tool failure is reported inside the tool result with isError set, as the MCP specification asks.
 *
 * @param[in] config Server configuration; mcpRoot bounds the paths tools may touch and info.locale picks the
 *        language of tool descriptions.
 * @param[in] in Request stream, one JSON message per line.
 * @param[out] out Response stream; reserved for protocol messages only.
 * @return Process exit code: 0 after a clean end of input.
 */
int RunMcpBridge(const config::ServerConfig& config, std::istream& in, std::ostream& out);

} // namespace angel_lsp::mcp
