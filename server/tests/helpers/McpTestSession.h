#pragma once

#include "helpers/TestUtils.h"
#include "lsp/mcp/McpBridge.h"
#include "lsp/mcp/McpJson.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <lsp/json/json.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace angel_lsp::test
{

/**
 * @brief A scripted MCP session driven through RunMcpBridge.
 *
 * Requests are numbered in the order they are added and the whole session is written up front: the
 * bridge answers in order and returns at end of input, so tests stay deterministic without waiting.
 * Each session owns a randomly named temporary root that is removed on destruction.
 */
struct McpTestSession
{
    std::string input;
    int nextId = 1;
    std::filesystem::path root;

    McpTestSession()
    {
        root = std::filesystem::temp_directory_path() / GenerateRandomSymbolName("mcp_root");
        std::filesystem::create_directories(root);
    }

    ~McpTestSession()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    McpTestSession(const McpTestSession&) = delete;
    McpTestSession& operator=(const McpTestSession&) = delete;

    /** @brief Queues a JSON-RPC request and returns its id. */
    int Request(const std::string& method, lsp::json::Value params)
    {
        lsp::json::Value message = mcp::json::MakeObject();
        message.object()["jsonrpc"] = std::string("2.0");
        message.object()["id"] = nextId;
        message.object()["method"] = std::string(method);
        message.object()["params"] = std::move(params);
        input += lsp::json::stringify(message) + "\n";
        return nextId++;
    }

    /** @brief Queues a tools/call request and returns its id. */
    int Call(const std::string& tool, lsp::json::Value arguments)
    {
        lsp::json::Value params = mcp::json::MakeObject();
        params.object()["name"] = std::string(tool);
        params.object()["arguments"] = std::move(arguments);
        return Request("tools/call", std::move(params));
    }

    /** @brief Runs the script and returns each reply keyed by its id. */
    std::map<int, lsp::json::Value> Run()
    {
        // Built the way the executable builds it, so a test sees the defaults a real agent session sees.
        const std::string rootArg = root.string();
        std::vector<std::string> arguments = {"angel_lsp", "--mcp", "--mcp-root", rootArg};
        std::vector<char*> argv;
        for (std::string& argument : arguments)
        {
            argv.push_back(argument.data());
        }
        const config::ServerConfig serverConfig = config::FromArgs(static_cast<int>(argv.size()), argv.data());
        std::istringstream in(input);
        std::ostringstream out;
        REQUIRE(mcp::RunMcpBridge(serverConfig, in, out) == 0);

        std::map<int, lsp::json::Value> replies;
        std::istringstream lines(out.str());
        std::string line;
        while (std::getline(lines, line))
        {
            lsp::json::Value reply = lsp::json::parse(line);
            const lsp::json::Value* id = reply.object().find("id");
            replies[(id != nullptr && id->isInteger()) ? id->integer() : 0] = std::move(reply);
        }
        return replies;
    }
};

/** @brief Builds `{path, text}` tool arguments. */
inline lsp::json::Value McpArgs(const std::string& path, const std::string& text)
{
    lsp::json::Value args = mcp::json::MakeObject();
    args.object()["path"] = std::string(path);
    args.object()["text"] = std::string(text);
    return args;
}

/** @brief Builds `{path}` tool arguments. */
inline lsp::json::Value McpPathArgs(const std::string& path)
{
    lsp::json::Value args = mcp::json::MakeObject();
    args.object()["path"] = std::string(path);
    return args;
}

/** @brief Builds `{path, line, character}` tool arguments. */
inline lsp::json::Value McpPositionArgs(const std::string& path, int line, int character)
{
    lsp::json::Value args = McpPathArgs(path);
    args.object()["line"] = line;
    args.object()["character"] = character;
    return args;
}

/** @brief Parses the JSON payload a tools/call reply carries as text, or returns the error text. */
inline lsp::json::Value McpPayload(const lsp::json::Value& reply, bool expectError = false)
{
    const lsp::json::Object& result = reply.object().get("result").object();
    CHECK(result.get("isError").boolean() == expectError);
    const std::string& text = result.get("content").array().front().object().get("text").string();
    return expectError ? lsp::json::Value(std::string(text)) : lsp::json::parse(text);
}

} // namespace angel_lsp::test
