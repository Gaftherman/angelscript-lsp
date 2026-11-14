#include "lsp/mcp/McpBridge.h"

#include "helpers/McpTestSession.h"
#include "helpers/TestUtils.h"
#include "lsp/mcp/McpJson.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <lsp/json/json.h>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

/**
 * Drives RunMcpBridge with a scripted stdin and reads its stdout, so every assertion is about what an
 * MCP client such as an agent would actually see. The whole session is written up front: the bridge
 * answers in order and returns at end of input, which makes the tests deterministic without any waiting.
 */
TEST_SUITE("lsp/McpBridge")
{

    TEST_CASE("McpBridge - Good - lists the nine tools and answers ping")
    {
        angel_lsp::test::McpTestSession session;
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
        angel_lsp::test::McpTestSession session;
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Holder");
        const std::string member = angel_lsp::test::GenerateRandomSymbolName("member");
        const std::string text = "class " + cls + " { int " + member + "; }\nvoid f() { " + cls + " c; c.";
        const std::string complete = text + "\n}\n";
        session.Call("open_document", angel_lsp::test::McpArgs("good.as", complete));

        const int hover = session.Call("hover", angel_lsp::test::McpPositionArgs("good.as", 0, 8));
        const int completion = session.Call(
            "completion",
            angel_lsp::test::McpPositionArgs("good.as", 1, static_cast<int>(text.size() - text.find('\n') - 1)));
        const auto replies = session.Run();

        CHECK_FALSE(angel_lsp::test::McpPayload(replies.at(hover)).isNull());
        const auto completed = angel_lsp::test::McpPayload(replies.at(completion));
        const auto& items = completed.object().get("items");
        bool found = false;
        for (const auto& item : items.array())
        {
            found = found || item.object().get("label").string() == member;
        }
        CHECK(found);
    }

    TEST_CASE("McpBridge - Bad - an error is reported on its range and an edit clears it")
    {
        angel_lsp::test::McpTestSession session;
        session.Call("open_document", angel_lsp::test::McpArgs("bad.as", "void f() {\n  int x = \"s\";\n}\n"));
        const int before = session.Call("diagnostics", angel_lsp::test::McpPathArgs("bad.as"));

        lsp::json::Value edit = angel_lsp::test::McpPathArgs("bad.as");
        edit.object()["startLine"] = 1;
        edit.object()["startCharacter"] = 10;
        edit.object()["endLine"] = 1;
        edit.object()["endCharacter"] = 13;
        edit.object()["text"] = std::string("1");
        session.Call("apply_edit", std::move(edit));
        const int after = session.Call("diagnostics", angel_lsp::test::McpPathArgs("bad.as"));
        const auto replies = session.Run();

        const auto errors = angel_lsp::test::McpPayload(replies.at(before));
        REQUIRE_FALSE(errors.array().empty());
        CHECK(errors.array().front().object().get("range").object().get("start").object().get("line").integer() == 1);
        const auto remaining = angel_lsp::test::McpPayload(replies.at(after));
        for (const auto& item : remaining.array())
        {
            CHECK(item.object().get("severity").integer() != 1);
        }
    }

    TEST_CASE("McpBridge - Bad - protocol and argument failures do not end the session")
    {
        angel_lsp::test::McpTestSession session;
        session.input += "{not json\n";
        const int unknown = session.Request("no/such/method", angel_lsp::mcp::json::MakeObject());
        const int missing = session.Call("hover", angel_lsp::test::McpPathArgs("x.as"));
        const int escape = session.Call("open_document", angel_lsp::test::McpArgs("../outside.as", "void f() {}"));
        const int notOpen = session.Call("apply_edit", angel_lsp::test::McpPathArgs("never_opened.as"));
        const int ping = session.Request("ping", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();

        CHECK(replies.at(0).object().get("error").object().get("code").integer() == -32700);
        CHECK(replies.at(unknown).object().get("error").object().get("code").integer() == -32601);
        angel_lsp::test::McpPayload(replies.at(missing), true);
        angel_lsp::test::McpPayload(replies.at(escape), true);
        angel_lsp::test::McpPayload(replies.at(notOpen), true);
        CHECK(replies.at(ping).object().contains("result"));
    }

    /** @brief Counts UTF-16 code units of UTF-8 text, the unit LSP columns use. */
    static int Utf16Length(const std::string& utf8)
    {
        int units = 0;
        for (const unsigned char c : utf8)
        {
            if ((c & 0xC0) == 0x80)
            {
                continue;
            }
            units += (c >= 0xF0) ? 2 : 1;
        }
        return units;
    }

    static std::string ErrorsOf(const lsp::json::Value& diagnostics)
    {
        std::string out;
        for (const auto& item : diagnostics.array())
        {
            if (item.object().get("severity").integer() == 1)
            {
                out += lsp::json::stringify(item);
            }
        }
        return out;
    }

    TEST_CASE("McpBridge - Good - signature help, definition and symbols agree with the source")
    {
        angel_lsp::test::McpTestSession session;
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("callee");
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Shape");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("area");
        const std::string text = "void " + fn + "(int a) {}\nclass " + cls + " { int " + method +
                                 "() { return 1; } }\nvoid g() { " + fn + "(1); }\n";
        session.Call("open_document", angel_lsp::test::McpArgs("sym.as", text));

        const int sig = session.Call(
            "signature_help", angel_lsp::test::McpPositionArgs("sym.as", 2, static_cast<int>(12 + fn.size() + 1)));
        const int def = session.Call("definition", angel_lsp::test::McpPositionArgs("sym.as", 2, 12));
        const int symbols = session.Call("document_symbols", angel_lsp::test::McpPathArgs("sym.as"));
        const auto replies = session.Run();

        const auto signature = angel_lsp::test::McpPayload(replies.at(sig));
        REQUIRE(signature.isObject());
        CHECK_FALSE(signature.object().get("signatures").array().empty());
        CHECK(lsp::json::stringify(angel_lsp::test::McpPayload(replies.at(def))).find("\"line\":0") !=
              std::string::npos);
        const std::string listed = lsp::json::stringify(angel_lsp::test::McpPayload(replies.at(symbols)));
        CHECK(listed.find(cls) != std::string::npos);
        CHECK(listed.find(method) != std::string::npos);
    }

    TEST_CASE("McpBridge - Good - replace_document and files read from disk change the diagnostics")
    {
        angel_lsp::test::McpTestSession session;
        std::ofstream(session.root / "disk.as") << "void f() {\n  int x = \"s\";\n}\n";
        session.Call("open_document", angel_lsp::test::McpPathArgs("disk.as"));
        const int broken = session.Call("diagnostics", angel_lsp::test::McpPathArgs("disk.as"));
        session.Call("replace_document", angel_lsp::test::McpArgs("disk.as", "void f() {\n  int x = 1;\n}\n"));
        const int fixed = session.Call("diagnostics", angel_lsp::test::McpPathArgs("disk.as"));
        const auto replies = session.Run();

        CHECK_FALSE(ErrorsOf(angel_lsp::test::McpPayload(replies.at(broken))).empty());
        CHECK(ErrorsOf(angel_lsp::test::McpPayload(replies.at(fixed))).empty());
    }

    TEST_CASE("McpBridge - Good - columns count UTF-16 units after multibyte text and CRLF")
    {
        angel_lsp::test::McpTestSession session;
        const std::string var = angel_lsp::test::GenerateRandomSymbolName("wide");
        const std::string prefix = "  string s = \"\xC3\xB1\xC3\xA9\"; int ";
        const std::string text = "void f() {\r\n" + prefix + var + " = 1; " + var + " = 2;\r\n}\r\n";
        session.Call("open_document", angel_lsp::test::McpArgs("wide.as", text));
        const int column = Utf16Length(prefix + var + " = 1; ");
        const int hover = session.Call("hover", angel_lsp::test::McpPositionArgs("wide.as", 1, column));
        const auto replies = session.Run();

        const auto hovered = angel_lsp::test::McpPayload(replies.at(hover));
        REQUIRE_FALSE(hovered.isNull());
        CHECK(lsp::json::stringify(hovered).find(var) != std::string::npos);
    }

    TEST_CASE("McpBridge - Bad - generated escape sequences never leave the root")
    {
        angel_lsp::test::McpTestSession session;
        const unsigned seed = std::random_device{}();
        INFO("seed " << seed);
        std::mt19937 rng(seed);

        std::vector<int> escapes;
        for (int i = 0; i < 20; ++i)
        {
            std::string path;
            const int down = static_cast<int>(rng() % 4);
            const int extra = static_cast<int>(rng() % 3);
            for (int d = 0; d < down; ++d)
            {
                path += (rng() % 2 == 0 ? "sub" : "./sub") + std::to_string(d) + "/";
            }
            for (int up = 0; up < down + 1 + extra; ++up)
            {
                path += "../";
            }
            escapes.push_back(session.Call("open_document", angel_lsp::test::McpArgs(path + "e.as", "void f() {}")));
        }
        const std::string outside = (session.root.parent_path() / "outside.as").generic_string();
        escapes.push_back(session.Call("open_document", angel_lsp::test::McpArgs(outside, "void f() {}")));
        const int inside = session.Call("open_document", angel_lsp::test::McpArgs("a/../ok.as", "void f() {}"));
        const auto replies = session.Run();

        for (const int id : escapes)
        {
            angel_lsp::test::McpPayload(replies.at(id), true);
        }
        angel_lsp::test::McpPayload(replies.at(inside));
    }

    TEST_CASE("McpBridge - Bad - tools on a document that was never opened fail at once")
    {
        angel_lsp::test::McpTestSession session;
        const int diag = session.Call("diagnostics", angel_lsp::test::McpPathArgs("ghost.as"));
        const int hover = session.Call("hover", angel_lsp::test::McpPositionArgs("ghost.as", 0, 0));
        const int symbols = session.Call("document_symbols", angel_lsp::test::McpPathArgs("ghost.as"));
        const auto replies = session.Run();

        for (const int id : {diag, hover, symbols})
        {
            const auto message = angel_lsp::test::McpPayload(replies.at(id), true);
            CHECK(message.string().find("not open") != std::string::npos);
        }
    }

    TEST_CASE("McpBridge - Bad - an unknown protocol version is answered with a supported one")
    {
        angel_lsp::test::McpTestSession session;
        lsp::json::Value params = angel_lsp::mcp::json::MakeObject();
        params.object()["protocolVersion"] = std::string("1999-01-01");
        const int init = session.Request("initialize", std::move(params));
        const auto replies = session.Run();

        CHECK(replies.at(init).object().get("result").object().get("protocolVersion").string() == "2025-06-18");
    }

    TEST_CASE("McpBridge - Normal - a completion limit keeps the first item by sort order and says so")
    {
        angel_lsp::test::McpTestSession session;
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Bag");
        std::string members;
        for (int i = 0; i < 3; ++i)
        {
            members += " int " + angel_lsp::test::GenerateRandomSymbolName("field") + ";";
        }
        const std::string head = "class " + cls + " {" + members + " }\nvoid f() { " + cls + " c; c.";
        session.Call("open_document", angel_lsp::test::McpArgs("limit.as", head + "\n}\n"));

        const std::string tail = head.substr(head.find('\n') + 1);
        lsp::json::Value args = angel_lsp::test::McpPositionArgs("limit.as", 1, static_cast<int>(tail.size()));
        const int all = session.Call("completion", args);
        args.object()["limit"] = 1;
        const int one = session.Call("completion", std::move(args));
        const auto replies = session.Run();

        const auto full = angel_lsp::test::McpPayload(replies.at(all));
        const auto limited = angel_lsp::test::McpPayload(replies.at(one));
        REQUIRE(full.object().get("items").array().size() >= 3);
        REQUIRE(limited.object().get("items").array().size() == 1);
        CHECK(limited.object().get("truncated").boolean());
        CHECK(limited.object().get("items").array().front().object().get("label").string() ==
              full.object().get("items").array().front().object().get("label").string());
    }

    TEST_CASE("McpBridge - Normal - a run of incremental edits matches the document opened with the final text")
    {
        angel_lsp::test::McpTestSession session;
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("run");
        session.Call("open_document", angel_lsp::test::McpArgs("typed.as", "void " + fn + "() {\n}\n"));

        // Types "int a = 1; a.nope;" one character at a time inside the body, the way a person would.
        const std::string typed = "int a = 1; a.nope;";
        std::string finalText = "void " + fn + "() {\n}\n";
        for (std::size_t i = 0; i < typed.size(); ++i)
        {
            lsp::json::Value edit = angel_lsp::test::McpPathArgs("typed.as");
            edit.object()["startLine"] = 1;
            edit.object()["startCharacter"] = static_cast<int>(i);
            edit.object()["endLine"] = 1;
            edit.object()["endCharacter"] = static_cast<int>(i);
            edit.object()["text"] = std::string(1, typed[i]);
            session.Call("apply_edit", std::move(edit));
        }
        finalText = "void " + fn + "() {\n" + typed + "}\n";
        const int incremental = session.Call("diagnostics", angel_lsp::test::McpPathArgs("typed.as"));

        session.Call("open_document", angel_lsp::test::McpArgs("fresh.as", finalText));
        const int fresh = session.Call("diagnostics", angel_lsp::test::McpPathArgs("fresh.as"));
        const auto replies = session.Run();

        const auto a = angel_lsp::test::McpPayload(replies.at(incremental));
        const auto b = angel_lsp::test::McpPayload(replies.at(fresh));
        REQUIRE_FALSE(a.array().empty());
        CHECK(lsp::json::stringify(a) == lsp::json::stringify(b));
    }
}
