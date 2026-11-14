#include "lsp/mcp/McpBridge.h"

#include "helpers/TestUtils.h"
#include "lsp/mcp/McpJson.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <lsp/json/json.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

/**
 * Drives RunMcpBridge with a scripted stdin and reads its stdout, so every assertion is about what an
 * MCP client such as an agent would actually see. The whole session is written up front: the bridge
 * answers in order and returns at end of input, which makes the tests deterministic without any waiting.
 */
TEST_SUITE("LspMcpBridge")
{

    /** @brief A scripted MCP session: requests are numbered in the order they are added. */
    struct Session
    {
        std::string input;
        int nextId = 1;
        std::filesystem::path root;

        Session()
        {
            root = std::filesystem::temp_directory_path() / angel_lsp::test::GenerateRandomSymbolName("mcp_root");
            std::filesystem::create_directories(root);
        }

        ~Session()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        int Request(const std::string& method, lsp::json::Value params)
        {
            lsp::json::Value message = angel_lsp::mcp::json::MakeObject();
            message.object()["jsonrpc"] = std::string("2.0");
            message.object()["id"] = nextId;
            message.object()["method"] = std::string(method);
            message.object()["params"] = std::move(params);
            input += lsp::json::stringify(message) + "\n";
            return nextId++;
        }

        int Call(const std::string& tool, lsp::json::Value arguments)
        {
            lsp::json::Value params = angel_lsp::mcp::json::MakeObject();
            params.object()["name"] = std::string(tool);
            params.object()["arguments"] = std::move(arguments);
            return Request("tools/call", std::move(params));
        }

        /** @brief Runs the script and returns each reply keyed by its id. */
        std::map<int, lsp::json::Value> Run()
        {
            angel_lsp::config::ServerConfig config;
            config.info.mcpRoot = root.string();
            std::istringstream in(input);
            std::ostringstream out;
            REQUIRE(angel_lsp::mcp::RunMcpBridge(config, in, out) == 0);

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

    static lsp::json::Value Args(const std::string& path, const std::string& text)
    {
        lsp::json::Value args = angel_lsp::mcp::json::MakeObject();
        args.object()["path"] = std::string(path);
        args.object()["text"] = std::string(text);
        return args;
    }

    static lsp::json::Value PathArgs(const std::string& path)
    {
        lsp::json::Value args = angel_lsp::mcp::json::MakeObject();
        args.object()["path"] = std::string(path);
        return args;
    }

    static lsp::json::Value PositionArgs(const std::string& path, int line, int character)
    {
        lsp::json::Value args = PathArgs(path);
        args.object()["line"] = line;
        args.object()["character"] = character;
        return args;
    }

    /** @brief Parses the JSON payload a tools/call reply carries as text. */
    static lsp::json::Value Payload(const lsp::json::Value& reply, bool expectError = false)
    {
        const lsp::json::Object& result = reply.object().get("result").object();
        CHECK(result.get("isError").boolean() == expectError);
        const std::string& text = result.get("content").array().front().object().get("text").string();
        return expectError ? lsp::json::Value(std::string(text)) : lsp::json::parse(text);
    }

    TEST_CASE("McpBridge - Good - lists the nine tools and answers ping")
    {
        Session session;
        const int list = session.Request("tools/list", angel_lsp::mcp::json::MakeObject());
        const int ping = session.Request("ping", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();

        const auto& tools = replies.at(list).object().get("result").object().get("tools").array();
        CHECK(tools.size() == 9);
        for (const auto& tool : tools)
        {
            CHECK_FALSE(tool.object().get("description").string().empty());
            CHECK(tool.object().get("inputSchema").object().get("type").string() == "object");
        }
        CHECK(replies.at(ping).object().contains("result"));
    }

    TEST_CASE("McpBridge - Good - open, hover and complete a member after a dot")
    {
        Session session;
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Holder");
        const std::string member = angel_lsp::test::GenerateRandomSymbolName("member");
        const std::string text = "class " + cls + " { int " + member + "; }\nvoid f() { " + cls + " c; c.";
        const std::string complete = text + "\n}\n";
        session.Call("open_document", Args("good.as", complete));

        const int hover = session.Call("hover", PositionArgs("good.as", 0, 8));
        const int completion =
            session.Call("completion", PositionArgs("good.as", 1, static_cast<int>(text.size() - text.find('\n') - 1)));
        const auto replies = session.Run();

        CHECK_FALSE(Payload(replies.at(hover)).isNull());
        const auto items = Payload(replies.at(completion));
        bool found = false;
        for (const auto& item : items.array())
        {
            found = found || item.object().get("label").string() == member;
        }
        CHECK(found);
    }

    TEST_CASE("McpBridge - Bad - an error is reported on its range and an edit clears it")
    {
        Session session;
        session.Call("open_document", Args("bad.as", "void f() {\n  int x = \"s\";\n}\n"));
        const int before = session.Call("diagnostics", PathArgs("bad.as"));

        lsp::json::Value edit = PathArgs("bad.as");
        edit.object()["startLine"] = 1;
        edit.object()["startCharacter"] = 10;
        edit.object()["endLine"] = 1;
        edit.object()["endCharacter"] = 13;
        edit.object()["text"] = std::string("1");
        session.Call("apply_edit", std::move(edit));
        const int after = session.Call("diagnostics", PathArgs("bad.as"));
        const auto replies = session.Run();

        const auto errors = Payload(replies.at(before));
        REQUIRE_FALSE(errors.array().empty());
        CHECK(errors.array().front().object().get("range").object().get("start").object().get("line").integer() == 1);
        const auto remaining = Payload(replies.at(after));
        for (const auto& item : remaining.array())
        {
            CHECK(item.object().get("severity").integer() != 1);
        }
    }

    TEST_CASE("McpBridge - Bad - protocol and argument failures do not end the session")
    {
        Session session;
        session.input += "{not json\n";
        const int unknown = session.Request("no/such/method", angel_lsp::mcp::json::MakeObject());
        const int missing = session.Call("hover", PathArgs("x.as"));
        const int escape = session.Call("open_document", Args("../outside.as", "void f() {}"));
        const int notOpen = session.Call("apply_edit", PathArgs("never_opened.as"));
        const int ping = session.Request("ping", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();

        CHECK(replies.at(0).object().get("error").object().get("code").integer() == -32700);
        CHECK(replies.at(unknown).object().get("error").object().get("code").integer() == -32601);
        Payload(replies.at(missing), true);
        Payload(replies.at(escape), true);
        Payload(replies.at(notOpen), true);
        CHECK(replies.at(ping).object().contains("result"));
    }

    TEST_CASE("McpBridge - Normal - a run of incremental edits matches the document opened with the final text")
    {
        Session session;
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("run");
        session.Call("open_document", Args("typed.as", "void " + fn + "() {\n}\n"));

        // Types "int a = 1; a.nope;" one character at a time inside the body, the way a person would.
        const std::string typed = "int a = 1; a.nope;";
        std::string finalText = "void " + fn + "() {\n}\n";
        for (std::size_t i = 0; i < typed.size(); ++i)
        {
            lsp::json::Value edit = PathArgs("typed.as");
            edit.object()["startLine"] = 1;
            edit.object()["startCharacter"] = static_cast<int>(i);
            edit.object()["endLine"] = 1;
            edit.object()["endCharacter"] = static_cast<int>(i);
            edit.object()["text"] = std::string(1, typed[i]);
            session.Call("apply_edit", std::move(edit));
        }
        finalText = "void " + fn + "() {\n" + typed + "}\n";
        const int incremental = session.Call("diagnostics", PathArgs("typed.as"));

        session.Call("open_document", Args("fresh.as", finalText));
        const int fresh = session.Call("diagnostics", PathArgs("fresh.as"));
        const auto replies = session.Run();

        const auto a = Payload(replies.at(incremental));
        const auto b = Payload(replies.at(fresh));
        REQUIRE_FALSE(a.array().empty());
        CHECK(lsp::json::stringify(a) == lsp::json::stringify(b));
    }
}
