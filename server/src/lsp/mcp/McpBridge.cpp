#include "lsp/mcp/McpBridge.h"

#include "lsp/mcp/LspClientSession.h"
#include "lsp/mcp/McpJson.h"
#include "lsp/mcp/McpTools.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <lsp/uri.h>
#include <string>

namespace angel_lsp::mcp
{

/// JSON-RPC error codes used by the bridge.
static constexpr int k_parseError = -32700;
static constexpr int k_invalidRequest = -32600;
static constexpr int k_methodNotFound = -32601;
static constexpr int k_invalidParams = -32602;

/// MCP protocol revisions the bridge speaks, newest first; the first is the fallback for unknown requests.
static constexpr std::array<std::string_view, 3> k_supportedProtocolVersions = {"2025-06-18", "2025-03-26",
                                                                                "2024-11-05"};

/** @brief Writes one message as a single line. */
static void WriteLine(std::ostream& out, const lsp::json::Value& message)
{
    out << lsp::json::stringify(message) << '\n';
    out.flush();
}

static lsp::json::Value MakeReply(const lsp::json::Value& id)
{
    lsp::json::Value reply = json::MakeObject();
    reply.object()["jsonrpc"] = std::string("2.0");
    reply.object()["id"] = id;
    return reply;
}

static lsp::json::Value MakeError(const lsp::json::Value& id, int code, std::string_view message)
{
    lsp::json::Value reply = MakeReply(id);
    lsp::json::Value error = json::MakeObject();
    error.object()["code"] = code;
    error.object()["message"] = std::string(message);
    reply.object()["error"] = std::move(error);
    return reply;
}

/** @brief Builds the tools/call result: the JSON payload as text, flagged when it is an error. */
static lsp::json::Value MakeToolResult(std::string text, bool isError)
{
    lsp::json::Value part = json::MakeObject();
    part.object()["type"] = std::string("text");
    part.object()["text"] = std::move(text);
    lsp::json::Array content;
    content.push_back(std::move(part));
    lsp::json::Value result = json::MakeObject();
    result.object()["content"] = std::move(content);
    result.object()["isError"] = isError;
    return result;
}

static lsp::json::Value MakeInitializeResult(const lsp::json::Value& params, const config::ServerConfig& config)
{
    std::string version(k_supportedProtocolVersions.front());
    if (params.isObject() && params.object().contains("protocolVersion") &&
        params.object().get("protocolVersion").isString())
    {
        const std::string& requested = params.object().get("protocolVersion").string();
        if (std::find(k_supportedProtocolVersions.begin(), k_supportedProtocolVersions.end(), requested) !=
            k_supportedProtocolVersions.end())
        {
            version = requested;
        }
    }
    lsp::json::Value result = json::MakeObject();
    result.object()["protocolVersion"] = std::move(version);
    lsp::json::Value capabilities = json::MakeObject();
    capabilities.object()["tools"] = json::MakeObject();
    result.object()["capabilities"] = std::move(capabilities);
    lsp::json::Value info = json::MakeObject();
    info.object()["name"] = std::string("angellsp");
    info.object()["version"] = std::string(config.info.version);
    result.object()["serverInfo"] = std::move(info);
    return result;
}

/** @brief Answers tools/call, turning every failure into an isError result. */
static lsp::json::Value HandleToolCall(McpToolbox& toolbox, const lsp::json::Value& params)
{
    try
    {
        const std::string name = json::GetString(params, "name", true);
        const lsp::json::Value* args = params.object().find("arguments");
        const lsp::json::Value payload = toolbox.CallTool(name, args != nullptr ? *args : lsp::json::Value());
        return MakeToolResult(lsp::json::stringify(payload), false);
    }
    catch (const std::exception& e)
    {
        return MakeToolResult(e.what(), true);
    }
}

int RunMcpBridge(const config::ServerConfig& config, std::istream& in, std::ostream& out)
{
    const std::string root =
        config.info.mcpRoot.empty() ? std::string() : std::filesystem::absolute(config.info.mcpRoot).generic_string();
    const std::string rootUri = root.empty() ? std::string() : lsp::Uri::fileUriFromPath(root).toString();

    LspClientSession session(config, rootUri);
    McpToolbox toolbox(session, root, config.info.locale);

    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line == "\r")
        {
            continue;
        }

        lsp::json::Value message;
        try
        {
            message = lsp::json::parse(line);
        }
        catch (const std::exception&)
        {
            WriteLine(out, MakeError(lsp::json::Value(), k_parseError, "parse error"));
            continue;
        }
        if (!message.isObject() || !message.object().contains("method") || !message.object().get("method").isString())
        {
            WriteLine(out, MakeError(lsp::json::Value(), k_invalidRequest, "invalid request"));
            continue;
        }

        const lsp::json::Object& obj = message.object();
        const std::string& method = obj.get("method").string();
        const lsp::json::Value* id = obj.find("id");
        if (id == nullptr)
        {
            continue; // Notifications such as notifications/initialized need no answer.
        }
        const lsp::json::Value* paramsPtr = obj.find("params");
        const lsp::json::Value params = paramsPtr != nullptr ? *paramsPtr : lsp::json::Value();

        lsp::json::Value reply = MakeReply(*id);
        if (method == "initialize")
        {
            reply.object()["result"] = MakeInitializeResult(params, config);
        }
        else if (method == "ping")
        {
            reply.object()["result"] = json::MakeObject();
        }
        else if (method == "tools/list")
        {
            lsp::json::Value result = json::MakeObject();
            result.object()["tools"] = toolbox.ListTools();
            reply.object()["result"] = std::move(result);
        }
        else if (method == "tools/call")
        {
            if (!params.isObject())
            {
                reply = MakeError(*id, k_invalidParams, "tools/call needs params");
            }
            else
            {
                reply.object()["result"] = HandleToolCall(toolbox, params);
            }
        }
        else
        {
            reply = MakeError(*id, k_methodNotFound, "method not found");
        }
        WriteLine(out, reply);
    }
    return 0;
}

} // namespace angel_lsp::mcp
