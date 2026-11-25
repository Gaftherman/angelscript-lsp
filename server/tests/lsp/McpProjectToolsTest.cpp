#include "helpers/McpTestSession.h"
#include "helpers/TestUtils.h"
#include "lsp/mcp/McpBridge.h"
#include "lsp/mcp/McpJson.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <lsp/json/json.h>
#include <string>
#include <vector>

/**
 * The project tools tell an agent what the workspace looks like before it edits: entry point, engine rules,
 * module files and the host API. Each test builds its workspace from generated names in the session's
 * temporary root and reads the tool results exactly as an MCP client would.
 */
static void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

static bool ContainsString(const lsp::json::Value& array, const std::string& wanted)
{
    for (const auto& item : array.array())
    {
        if (item.isString() && item.string() == wanted)
        {
            return true;
        }
    }
    return false;
}

TEST_SUITE("lsp/McpProjectTools")
{

    TEST_CASE("McpProjectTools - Good - the six project tools are listed with descriptions")
    {
        angel_lsp::test::McpTestSession session;
        const int list = session.Request("tools/list", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();

        const auto& tools = replies.at(list).object().get("result").object().get("tools").array();
        CHECK(tools.size() == 15);
        for (const char* wanted :
             {"workspace_info", "engine_rules", "module_graph", "references", "workspace_symbols", "predefined_lookup"})
        {
            bool found = false;
            for (const auto& tool : tools)
            {
                if (tool.object().get("name").string() == wanted)
                {
                    found = true;
                    CHECK_FALSE(tool.object().get("description").string().empty());
                }
            }
            CHECK_MESSAGE(found, wanted);
        }
    }

    TEST_CASE("McpProjectTools - Good - workspace_info reports the root and the rules file in effect")
    {
        angel_lsp::test::McpTestSession session;
        const std::string rulesName = angel_lsp::test::GenerateRandomSymbolName("host") + ".angelscript.rules.json";
        WriteFile(session.root / rulesName, R"({"name":"generated","engineProperties":{"requireEnumScope":true}})");

        const int info = session.Call("workspace_info", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();
        const auto payload = angel_lsp::test::McpPayload(replies.at(info));

        CHECK(payload.object().get("root").isString());
        CHECK(payload.object().get("modules").isArray());
        CHECK(payload.object().get("activePredefined").isArray());
        CHECK(payload.object().get("includeRoots").isArray());
        CHECK(payload.object().get("rulesFile").isString());
        CHECK(payload.object().get("rulesFile").string() == rulesName);
    }

    TEST_CASE("McpProjectTools - Good - workspace_info warns about every missing piece in an empty workspace")
    {
        angel_lsp::test::McpTestSession session;
        const int info = session.Call("workspace_info", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();
        const auto payload = angel_lsp::test::McpPayload(replies.at(info));

        std::vector<std::string> codes;
        for (const auto& warning : payload.object().get("warnings").array())
        {
            codes.push_back(warning.object().get("code").string());
            CHECK_FALSE(warning.object().get("message").string().empty());
            CHECK_FALSE(warning.object().get("ask").string().empty());
        }
        CHECK(std::find(codes.begin(), codes.end(), "no-rules-file") != codes.end());
        CHECK(std::find(codes.begin(), codes.end(), "no-predefined") != codes.end());
        CHECK(std::find(codes.begin(), codes.end(), "no-module-entry-point") != codes.end());
        CHECK_FALSE(payload.object().get("nextSteps").array().empty());
    }

    TEST_CASE("McpProjectTools - Good - workspace_info drops the rules and predefined warnings once they exist")
    {
        angel_lsp::test::McpTestSession session;
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Host");
        WriteFile(session.root / "host.angelscript.rules.json", R"({"name":"generated"})");
        WriteFile(session.root / "host.as.predefined", "class " + typeName + " {}\n");

        const int info = session.Call("workspace_info", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();
        const auto payload = angel_lsp::test::McpPayload(replies.at(info));

        for (const auto& warning : payload.object().get("warnings").array())
        {
            const std::string code = warning.object().get("code").string();
            CHECK(code != "no-rules-file");
            CHECK(code != "no-predefined");
        }
    }

    TEST_CASE("McpProjectTools - Good - engine_rules returns the engine profile and warns about newer properties")
    {
        angel_lsp::test::McpTestSession session;
        WriteFile(session.root / "host.angelscript.rules.json",
                  R"({"engine":{"name":"host","version":"2.36.1"},"engineProperties":{"foreachSupport":true}})");

        const int rules = session.Call("engine_rules", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();
        const auto payload = angel_lsp::test::McpPayload(replies.at(rules));

        const auto& engine = payload.object().get("engine").object();
        CHECK(engine.get("version").string() == "2.36.1");
        CHECK(engine.get("source").string() == "official");
        CHECK(engine.get("declared").boolean());
        CHECK(engine.get("latestSupported").string() == "2.39.0");
        REQUIRE(engine.get("warnings").array().size() == 1);
        CHECK(engine.get("warnings").array().front().string().find("foreachSupport") != std::string::npos);
    }

    TEST_CASE("McpProjectTools - Good - engine_rules returns the properties and the storage rule that were written")
    {
        angel_lsp::test::McpTestSession session;
        const std::string ruleId = angel_lsp::test::GenerateRandomSymbolName("rule");
        const std::string typeName = angel_lsp::test::GenerateRandomSymbolName("Entity");
        WriteFile(session.root / "host.angelscript.rules.json",
                  R"({"name":"generated","engineProperties":{"requireEnumScope":true,"propertyAccessorMode":2},)"
                  R"("storageRules":[{"id":")" +
                      ruleId + R"(","types":[")" + typeName +
                      R"("],"disallowInMembers":true,"suggestReplacement":"SafeHandle","severity":"error"}]})");

        const int rules = session.Call("engine_rules", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();
        const auto payload = angel_lsp::test::McpPayload(replies.at(rules));

        CHECK(payload.object().get("name").string() == "generated");
        const auto& properties = payload.object().get("engineProperties").object();
        CHECK(properties.get("requireEnumScope").boolean());
        CHECK(properties.get("propertyAccessorMode").integer() == 2);
        const auto& storage = payload.object().get("storageRules").array();
        REQUIRE(storage.size() == 1);
        CHECK(storage.front().object().get("id").string() == ruleId);
        CHECK(ContainsString(storage.front().object().get("types"), typeName));
        CHECK(storage.front().object().get("severity").string() == "error");
        CHECK(payload.object().get("callRules").isArray());
        CHECK(payload.object().get("unusedSymbolRules").isObject());
    }

    TEST_CASE("McpProjectTools - Good - module_graph follows includes from the given script and honours the limit")
    {
        angel_lsp::test::McpTestSession session;
        const std::string first = angel_lsp::test::GenerateRandomSymbolName("first") + ".as";
        const std::string second = angel_lsp::test::GenerateRandomSymbolName("second") + ".as";
        const std::string entry = angel_lsp::test::GenerateRandomSymbolName("entry") + ".as";
        WriteFile(session.root / first, "void a() {}\n");
        WriteFile(session.root / second, "void b() {}\n");
        WriteFile(session.root / entry, "#include \"" + first + "\"\n#include \"" + second + "\"\nvoid main() {}\n");

        session.Call("open_document", angel_lsp::test::McpPathArgs(entry));
        const int graph = session.Call("module_graph", angel_lsp::test::McpPathArgs(entry));
        lsp::json::Value limited = angel_lsp::test::McpPathArgs(entry);
        limited.object()["limit"] = 1;
        const int small = session.Call("module_graph", std::move(limited));
        const auto replies = session.Run();

        const auto full = angel_lsp::test::McpPayload(replies.at(graph));
        CHECK(full.object().get("entry").string() == entry);
        CHECK(ContainsString(full.object().get("files"), first));
        CHECK(ContainsString(full.object().get("files"), second));
        CHECK_FALSE(full.object().get("truncated").boolean());

        const auto cut = angel_lsp::test::McpPayload(replies.at(small));
        CHECK(cut.object().get("files").array().size() == 1);
        CHECK(cut.object().get("truncated").boolean());
    }

    TEST_CASE("McpProjectTools - Good - predefined_lookup finds a host class declared in a stub")
    {
        angel_lsp::test::McpTestSession session;
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("HostClass");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("method");
        WriteFile(session.root / "host.as.predefined",
                  "/** Generated host class. */\nclass " + cls + "\n{\n    int " + method + "() const;\n}\n");

        lsp::json::Value args = angel_lsp::mcp::json::MakeObject();
        args.object()["name"] = std::string(cls);
        const int lookup = session.Call("predefined_lookup", std::move(args));
        lsp::json::Value missing = angel_lsp::mcp::json::MakeObject();
        missing.object()["name"] = angel_lsp::test::GenerateRandomSymbolName("Absent");
        const int none = session.Call("predefined_lookup", std::move(missing));
        const auto replies = session.Run();

        const auto found = angel_lsp::test::McpPayload(replies.at(lookup));
        REQUIRE(found.object().get("matches").array().size() == 1);
        const auto& match = found.object().get("matches").array().front().object();
        CHECK(match.get("kind").string() == "class");
        CHECK(match.get("qualifiedName").string() == cls);
        CHECK(match.get("declaration").string().find(cls) != std::string::npos);
        CHECK(match.get("file").string() == "host.as.predefined");
        CHECK(match.get("documentation").string().find("Generated host class.") != std::string::npos);

        const auto absent = angel_lsp::test::McpPayload(replies.at(none));
        CHECK(absent.object().get("matches").array().empty());
    }

    TEST_CASE("McpProjectTools - Good - references and workspace_symbols agree with the source")
    {
        angel_lsp::test::McpTestSession session;
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Counted");
        const std::string text = "class " + cls + " {}\nvoid f() { " + cls + " a; " + cls + " b; }\n";
        session.Call("open_document", angel_lsp::test::McpArgs("refs.as", text));

        lsp::json::Value args = angel_lsp::test::McpPositionArgs("refs.as", 0, 8);
        const int refs = session.Call("references", args);
        args.object()["includeDeclaration"] = false;
        const int usesOnly = session.Call("references", std::move(args));
        lsp::json::Value query = angel_lsp::mcp::json::MakeObject();
        query.object()["query"] = std::string(cls);
        const int symbols = session.Call("workspace_symbols", std::move(query));
        const auto replies = session.Run();

        const auto all = angel_lsp::test::McpPayload(replies.at(refs));
        const auto uses = angel_lsp::test::McpPayload(replies.at(usesOnly));
        REQUIRE(all.isArray());
        REQUIRE(uses.isArray());
        CHECK(all.array().size() == uses.array().size() + 1);

        const auto found = angel_lsp::test::McpPayload(replies.at(symbols));
        CHECK_FALSE(found.object().get("truncated").boolean());
        bool listed = false;
        for (const auto& item : found.object().get("items").array())
        {
            listed = listed || item.object().get("name").string() == cls;
        }
        CHECK(listed);
    }

    TEST_CASE("McpProjectTools - Bad - missing and mistyped arguments are tool errors, not session failures")
    {
        angel_lsp::test::McpTestSession session;
        const int noQuery = session.Call("workspace_symbols", angel_lsp::mcp::json::MakeObject());
        const int noName = session.Call("predefined_lookup", angel_lsp::mcp::json::MakeObject());
        lsp::json::Value mistyped = angel_lsp::test::McpPositionArgs("never-opened.as", 0, 0);
        mistyped.object()["includeDeclaration"] = std::string("yes");
        const int badFlag = session.Call("references", std::move(mistyped));
        const int alive = session.Request("ping", angel_lsp::mcp::json::MakeObject());
        const auto replies = session.Run();

        for (const int id : {noQuery, noName, badFlag})
        {
            CHECK(angel_lsp::test::McpPayload(replies.at(id), true).string().size() > 0);
        }
        CHECK(replies.at(alive).object().contains("result"));
    }

} // TEST_SUITE
