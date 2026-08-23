#include <doctest/doctest.h>

#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace angel_lsp;

// =====================================================================================
// End-to-end coverage of the Layer 4 orchestrator.
//
// Everything else in this suite tests a pure function. Server is not one: it owns a JSON-RPC
// connection, two background threads and the whole document index, and its handlers are reachable
// only through the message loop. Speaking the protocol at it over an in-memory stream is what makes
// the notification handlers - workspace/didChangeWatchedFiles above all - testable at all.
// =====================================================================================

namespace
{
    /** @brief A throwaway workspace directory with real files on disk.
     *  @note Real files rather than an injected reader: the include graph resolves a directive by
     *        asking the filesystem whether the target exists, and the watched-files handler reads
     *        changed files off disk. Both would see nothing in an in-memory fixture. */
    struct WorkspaceFixture
    {
        std::filesystem::path dir;

        WorkspaceFixture()
        {
            const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            dir = std::filesystem::temp_directory_path() / ("angel_lsp_server_" + unique);
            std::filesystem::create_directories(dir);
        }

        ~WorkspaceFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &contents) const
        {
            std::ofstream out(dir / name, std::ios::binary);
            out << contents;
        }

        /** @brief file:// URI of a workspace file, in the spelling a client would send. */
        std::string Uri(const std::string &name) const
        {
            std::string path = (dir / name).string();
            std::replace(path.begin(), path.end(), '\\', '/');
            return "file:///" + path;
        }

        std::string RootUri() const
        {
            std::string path = dir.string();
            std::replace(path.begin(), path.end(), '\\', '/');
            return "file:///" + path;
        }
    };

    /** @brief Drives a Server through a scripted message sequence and returns everything it wrote. */
    std::string RunScript(const config::ServerConfig &config, test::ScriptedStream &stream)
    {
        Server server(config, stream);
        server.Run();
        return stream.Output();
    }

    std::string InitializeMessage(const std::string &rootUri)
    {
        return R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
               R"("processId":null,"rootUri":")" + rootUri + R"(",)"
               R"("capabilities":{},)"
               R"("workspaceFolders":[{"uri":")" + rootUri + R"(","name":"fixture"}]}})";
    }

    std::string DidOpenMessage(const std::string &uri, const std::string &text)
    {
        return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
               R"("uri":")" + uri + R"(","languageId":"angelscript","version":1,"text":")" + text + R"("}}})";
    }
}

TEST_CASE("Server - Announces the capabilities its feature flags enable")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    const std::string output = RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"hoverProvider\""));
    CHECK(stream.OutputContains("\"documentLinkProvider\""));
    CHECK(stream.OutputContains("\"semanticTokensProvider\""));

    // Both added in this round of work: the viewport-sized token request, and the folder-change
    // notification the include graph depends on to see a folder added mid-session.
    CHECK(stream.OutputContains("\"range\""));
    CHECK(stream.OutputContains("\"workspaceFolders\""));
    CHECK(stream.OutputContains("\"changeNotifications\""));
}

TEST_CASE("Server - A disabled feature flag withholds its capability")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enableHover = false;
    serverConfig.features.enableDocumentLink = false;
    RunScript(serverConfig, stream);

    CHECK_FALSE(stream.OutputContains("\"hoverProvider\""));
    CHECK_FALSE(stream.OutputContains("\"documentLinkProvider\""));
    CHECK(stream.OutputContains("\"definitionProvider\""));
}

TEST_CASE("Server - Publishes diagnostics for an opened document")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() { int unused = 1; }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), "void main() { int unused = 1; }"));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("textDocument/publishDiagnostics"));
    CHECK(stream.OutputContains("as-warn-unused-variable"));
}

TEST_CASE("Server - A watched file deleted on disk stops contributing symbols")
{
    WorkspaceFixture fixture;
    fixture.Write("helper.as", "void Helper() {}\n");
    fixture.Write("main.as", "#include \"helper.as\"\nvoid main() { Helper(); }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    const std::string mainText = "#include \\\"helper.as\\\"\\nvoid main() { Helper(); }";
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainText));

    // A save, not just an open: didSave patches the include graph synchronously before computing
    // the module closure, whereas the graph an open relies on is built by the background workspace
    // scan. Without this the test would race that scan.
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"text":")" + mainText + R"("}})");

    // Asked before and after, because the effect of the deletion is a change to the index rather
    // than anything the server volunteers: the re-analysis it schedules runs on the debounced
    // background thread and would race the end of the script.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"Helper"}})");

    // The file leaves the disk, and the editor tells the server about it. Without the watched-files
    // handler nothing would reach the index until main.as happened to be reopened.
    // Scheduled rather than executed inline: the whole script is built before the server starts.
    stream.PushAction([dir = fixture.dir]() { std::filesystem::remove(dir / "helper.as"); });
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
                R"({"uri":")" + fixture.Uri("helper.as") + R"(","type":3}]}})");

    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"workspace/symbol","params":{"query":"Helper"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // Present in the first answer, gone from the second: exactly one mention across both.
    CHECK(stream.CountInOutput("\"name\":\"Helper\"") == 1);
}

TEST_CASE("Server - Survives a watched-file event naming a path it never indexed")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
                R"({"uri":")" + fixture.Uri("never-existed.as") + R"(","type":3},)"
                R"({"uri":")" + fixture.Uri("also-missing.as") + R"(","type":1}]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - Accepts a workspace folder added after initialize")
{
    WorkspaceFixture first;
    first.Write("main.as", "void main() {}\n");

    WorkspaceFixture second;
    second.Write("extra.as", "void Extra() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(first.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeWorkspaceFolders","params":{"event":{)"
                R"("added":[{"uri":")" + second.RootUri() + R"(","name":"second"}],"removed":[]}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - Loads a predefined stub named by configuration from outside the workspace")
{
    // The stub deliberately lives in its own directory, outside every workspace folder: that is
    // exactly the case the workspace scan cannot reach and --predefined-file exists for.
    WorkspaceFixture workspace;
    workspace.Write("main.as", "void main() {}\n");

    WorkspaceFixture elsewhere;
    elsewhere.Write("engine.as.predefined", "class CBaseEntity { void Spawn(); }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(workspace.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(workspace.Uri("main.as"), "void main() { CBaseEntity@ e; }"));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.predefinedFiles.push_back((elsewhere.dir / "engine.as.predefined").string());

    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - Run returns instead of throwing when the transport closes")
{
    // The script ends without a shutdown request, which is what an editor being killed looks like.
    // Before this was handled the exception escaped Run(), so the background threads were never
    // joined and an ordinary exit read as a crash.
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - Answers a semantic token delta against the payload it last sent")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), "void main() {}"));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{)"
                R"("textDocument":{"uri":")" + fixture.Uri("main.as") + R"("}}})");
    // The id the server minted for the payload above is "1": the counter starts at zero and this
    // is the first token stream of the session.
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/semanticTokens/full/delta","params":{)"
                R"("textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"previousResultId":"1"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains(R"("id":2,"result":{"data":)"));
    CHECK(stream.OutputContains(R"("resultId":"1")"));

    // Nothing changed between the two requests, so the delta is an empty edit list rather than a
    // second copy of the whole stream.
    CHECK(stream.OutputContains(R"("id":3,"result":{"edits":[],"resultId":"2"})"));
}

TEST_CASE("Server - Falls back to a full stream when the delta base is unknown")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), "void main() {}"));
    // A result id from some past session. Answering with edits against it would corrupt whatever
    // the client is holding, so the protocol allows a full stream instead.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full/delta","params":{)"
                R"("textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"previousResultId":"stale"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains(R"("id":2,"result":{"data":)"));
    CHECK_FALSE(stream.OutputContains(R"("edits")"));
}
