#include <doctest/doctest.h>

#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"

#include "utils/Utils.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
            std::error_code ec;
            auto c = std::filesystem::canonical(dir, ec);
            if (!ec)
                dir = std::move(c);
        }

        ~WorkspaceFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void Write(const std::string &name, const std::string &contents) const
        {
            // Parent directories created first. Without this a fixture writing "scripts/maps/x.as"
            // silently wrote nothing at all, and the test that read it back was measuring an empty
            // workspace while passing its earlier assertions.
            const std::filesystem::path full = dir / name;
            if (full.has_parent_path())
            {
                std::error_code ec;
                std::filesystem::create_directories(full.parent_path(), ec);
            }

            std::ofstream out(full, std::ios::binary);
            out << contents;
        }

        /** @brief file:// URI of a workspace file, in the spelling a client would send. */
        std::string Uri(const std::string &name) const
        {
            return angel_lsp::utils::PathToUri((dir / name).string());
        }

        std::string RootUri() const
        {
            return angel_lsp::utils::PathToUri(dir.string());
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

    /** @brief Escapes a document so it can be carried inside a JSON string literal. */
    std::string JsonEscape(const std::string &text)
    {
        std::string escaped;
        escaped.reserve(text.size() + 16);
        for (const char c : text)
        {
            switch (c)
            {
            case '"':  escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:   escaped.push_back(c); break;
            }
        }
        return escaped;
    }

    std::string DidOpenMessage(const std::string &uri, const std::string &text)
    {
        return R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{)"
               R"("uri":")" + uri + R"(","languageId":"angelscript","version":1,"text":")" +
               JsonEscape(text) + R"("}}})";
    }

    /**
     * @brief Opens one document and returns everything the server wrote back.
     *
     * The shape every rule-module test below shares: initialize, open a document written to trip
     * one module's rules, shut down. What it proves is the part unit tests cannot - that a
     * diagnostic survives the trip through publishDiagnostics and reaches the client at all.
     */
    std::string DiagnosticsFor(const std::string &source, config::ServerConfig serverConfig = {})
    {
        WorkspaceFixture fixture;
        fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeMessage(fixture.RootUri()));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        RunScript(serverConfig, stream);
        return stream.Output();
    }

    /**
     * @brief Just the publishDiagnostics frames of a transcript, concatenated.
     *
     * Searching the whole transcript is not good enough: the server also writes window/logMessage
     * notifications that quote the rule name and the diagnostic code verbatim, so a test looking
     * for a code would pass on the debug log alone while the diagnostic never reached the client -
     * which is the one thing these tests exist to prove.
     */
    std::string PublishedFrames(const std::string &output)
    {
        std::string frames;
        size_t pos = 0;
        while (pos < output.size())
        {
            const size_t headerStart = output.find("Content-Length:", pos);
            if (headerStart == std::string::npos)
                break;

            const size_t bodyStart = output.find("\r\n\r\n", headerStart);
            if (bodyStart == std::string::npos)
                break;

            const size_t contentStart = bodyStart + 4;
            const size_t nextHeader = output.find("Content-Length:", contentStart);
            const size_t bodyLength = (nextHeader == std::string::npos) ? (output.size() - contentStart) : (nextHeader - contentStart);

            std::string body = output.substr(contentStart, bodyLength);
            if (body.find("textDocument/publishDiagnostics") != std::string::npos)
            {
                frames += body;
            }

            pos = contentStart + bodyLength;
        }
        return frames;
    }

    /** @brief True when a diagnostic carrying this code was published to the client. */
    bool Published(const std::string &output, const std::string &code)
    {
        return PublishedFrames(output).find("\"" + code + "\"") != std::string::npos;
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

TEST_CASE("Server - Announces the navigation capabilities added in this round")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"declarationProvider\""));
    CHECK(stream.OutputContains("\"implementationProvider\""));
    CHECK(stream.OutputContains("\"selectionRangeProvider\""));
}

TEST_CASE("Server - Withholds the new capabilities when their flags are off")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enableImplementation = false;
    serverConfig.features.enableSelectionRange = false;
    RunScript(serverConfig, stream);

    CHECK_FALSE(stream.OutputContains("\"implementationProvider\""));
    CHECK_FALSE(stream.OutputContains("\"selectionRangeProvider\""));

    // Declaration rides on the definition flag, because it is the same handler.
    CHECK(stream.OutputContains("\"declarationProvider\""));
}

TEST_CASE("Server - Answers an implementation request over the wire")
{
    const std::string source =
        "interface IThinker\n"
        "{\n"
        "    void Think();\n"
        "}\n"
        "class Robot : IThinker\n"
        "{\n"
        "    void Think() { }\n"
        "}\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/implementation","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"position":{"line":0,"character":12}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // The implementing class starts on line 4; an answer that is null or empty would not carry it.
    CHECK(stream.OutputContains("\"line\":4"));
}

TEST_CASE("Server - Answers a selection range request over the wire")
{
    const std::string source = "void Think() { int ticks = 0; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/selectionRange","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"positions":[{"line":0,"character":20}]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // A chain, not a single range: the nesting is the whole answer.
    CHECK(stream.OutputContains("\"parent\""));
}

TEST_CASE("Server - Announces the hierarchy capabilities")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"callHierarchyProvider\""));
    CHECK(stream.OutputContains("\"typeHierarchyProvider\""));
}

TEST_CASE("Server - Answers a call hierarchy over the wire")
{
    const std::string source =
        "void Helper() { }\n"
        "void Spawn() { Helper(); }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/prepareCallHierarchy","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"position":{"line":0,"character":6}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // The qualified name travels in `detail` and in `data`, which is what the follow-up requests
    // need to tell one `Think` from another.
    CHECK(stream.OutputContains("\"Helper\""));
    CHECK(stream.OutputContains("\"data\""));
}

TEST_CASE("Server - Answers a type hierarchy over the wire")
{
    const std::string source =
        "class Base { }\n"
        "class Derived : Base { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/prepareTypeHierarchy","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"position":{"line":1,"character":8}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"Derived\""));
}

TEST_CASE("Server - Announces and answers linked editing")
{
    const std::string source =
        "void main()\n"
        "{\n"
        "    int ticks = 0;\n"
        "    ticks = ticks + 1;\n"
        "}\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/linkedEditingRange","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"position":{"line":2,"character":9}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"linkedEditingRangeProvider\""));
    CHECK(stream.OutputContains("\"ranges\""));
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
    const std::string mainText = "#include \"helper.as\"\nvoid main() { Helper(); }";
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainText));

    // A save, not just an open: didSave patches the include graph synchronously before computing
    // the module closure, whereas the graph an open relies on is built by the background workspace
    // scan. Without this the test would race that scan.
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"text":")" + JsonEscape(mainText) + R"("}})");

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

    // Asserted per reply rather than over the whole transcript: the server also writes log
    // notifications and background-thread diagnostics, so counting across everything would make
    // this depend on thread timing.
    CHECK(stream.ResponseFor(2).find("\"name\":\"Helper\"") != std::string::npos);
    CHECK(stream.ResponseFor(3).find("\"name\":\"Helper\"") == std::string::npos);
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

    const std::string r2 = stream.ResponseFor(2);
    CHECK(r2.find("\"data\"") != std::string::npos);
    CHECK(r2.find("\"resultId\":\"1\"") != std::string::npos);

    // Nothing changed between the two requests, so the delta is an empty edit list rather than a
    // second copy of the whole stream.
    const std::string r3 = stream.ResponseFor(3);
    CHECK(r3.find("\"edits\":[]") != std::string::npos);
    CHECK(r3.find("\"resultId\":\"2\"") != std::string::npos);
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

    const std::string r2 = stream.ResponseFor(2);
    CHECK(r2.find("\"data\"") != std::string::npos);
    CHECK(r2.find("\"edits\"") == std::string::npos);
}

// =====================================================================================
// Every rule module, over the protocol
//
// The unit tests prove a rule fires; these prove the finding reaches the client. Nothing between
// the analyzer and publishDiagnostics was covered before - a code could be dropped by the severity
// mapping, the debounce or the serializer and every unit test would still pass.
//
// One document per module rather than one carrying every error: rules interact, and a class that
// is simultaneously final, abstract and missing an interface method stops being a test of anything
// in particular.
// =====================================================================================

TEST_CASE("Server - Publishes the class rule diagnostics")
{
    const std::string source =
        "mixin final class Helper {}\n"
        "final class Sealed {}\n"
        "class Derived : Sealed {}\n"
        "interface IThink { void Think(); }\n"
        "class Idle : IThink {}\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-mixin-final"));
    CHECK(Published(output, "as-err-inherit-final"));
    CHECK(Published(output, "as-err-interface-impl-missing"));
}

TEST_CASE("Server - Publishes the type rule diagnostics")
{
    // A floating point initializer, not an identifier one: referring to a constant is legal and the
    // rule deliberately leaves it alone.
    const std::string source =
        "enum Mode { First = 1, Second = 1.5 }\n"
        "void Repeated() {}\n"
        "void Repeated() {}\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-enum-invalid-initializer"));
    CHECK(Published(output, "as-err-duplicate-symbol"));
}

TEST_CASE("Server - Publishes the variable rule diagnostics")
{
    const std::string source =
        "void g_nothing;\n"
        "int@ g_broken;\n"
        "private int g_scoped;\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-void-variable"));
    CHECK(Published(output, "as-err-handle-on-primitive"));
    CHECK(Published(output, "as-err-global-variable-access-modifier"));
}

TEST_CASE("Server - Publishes the function rule diagnostics")
{
    const std::string source =
        "void Orphan();\n"
        "void Move(int x, int x) {}\n"
        "void Think() const {}\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-missing-body"));
    CHECK(Published(output, "as-err-duplicate-param"));
    CHECK(Published(output, "as-err-global-function-qualifiers"));
}

TEST_CASE("Server - Publishes the operator rule diagnostics")
{
    const std::string source =
        "class Vec\n"
        "{\n"
        "    float opCmp(const Vec &in other) const { return 0; }\n"
        "    Vec opAdd() const { return this; }\n"
        "}\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-opcmp-return-int"));
    CHECK(Published(output, "as-err-binary-operator-arity"));
}

TEST_CASE("Server - Publishes the control flow diagnostics")
{
    const std::string source =
        "void Loose() { break; }\n"
        "int Silent() { int x = 1; }\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-break-outside-loop"));
    CHECK(Published(output, "as-err-not-all-paths-return"));
}

TEST_CASE("Server - Publishes the member access diagnostics")
{
    // The case that started this rule, end to end: a user writing it in the editor should see it.
    const std::string source =
        "class MyClass\n"
        "{\n"
        "    private float f;\n"
        "    protected int p;\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    MyClass myClass;\n"
        "    myClass.f = 3.0f;\n"
        "    myClass.p = 1;\n"
        "}\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-private-member-access"));
    CHECK(Published(output, "as-err-protected-member-access"));
}

TEST_CASE("Server - Publishes the function attribute diagnostics")
{
    const std::string source =
        "class Entity\n"
        "{\n"
        "    void Think() delete;\n"
        "    void Broken() property { }\n"
        "}\n"
        "void Convert() explicit { }\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-delete-not-auto-generated"));
    CHECK(Published(output, "as-err-virtual-property-signature"));
    CHECK(Published(output, "as-err-explicit-not-member"));
}

TEST_CASE("Server - Publishes the diagnostics the widened grammar made reachable")
{
    // Each of these used to reach the user as `Syntax error: "<token>"`, because the grammar
    // refused the construct. They now arrive as sentences, which is the whole point of parsing
    // something the engine rejects.
    const std::string source =
        "class Entity {}\n"
        "typedef Entity Alias;\n"
        "interface IThing { IThing(); void Do(); }\n"
        "funcdef void Callback() delete;\n"
        "array<void> g_bad;\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-typedef-non-primitive"));
    CHECK(Published(output, "as-err-interface-constructor"));
    CHECK(Published(output, "as-err-funcdef-attribute"));
    CHECK(Published(output, "as-err-array-invalid-template"));
}

TEST_CASE("Server - Publishes the non-instantiable type diagnostics")
{
    const std::string source =
        "abstract class Shape { void Draw() {} }\n"
        "interface IThing { void Do(); }\n"
        "Shape g_shape;\n"
        "IThing g_thing;\n"
        "void Take(Shape s) { }\n"
        "Shape Make() { return Shape(); }\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-abstract-instantiated"));
    CHECK(Published(output, "as-err-interface-instantiated"));
    CHECK(Published(output, "as-err-parameter-not-instantiable"));
    CHECK(Published(output, "as-err-return-not-instantiable"));
}

TEST_CASE("Server - Publishes the const correctness diagnostics")
{
    const std::string source =
        "const int g_max = 10;\n"
        "class Entity { int v; void Mutate() { v = 1; } }\n"
        "void main() { g_max = 5; }\n"
        "void Take(const Entity &in e) { e.Mutate(); }\n";

    const std::string output = DiagnosticsFor(source);
    CHECK(Published(output, "as-err-const-assignment"));
    CHECK(Published(output, "as-err-const-method-required"));
}

TEST_CASE("Server - An engine property reaches the rules that depend on it")
{
    // The end of the wire the configuration exists for. A rule whose answer lives in the host's
    // SetEngineProperty call is only useful if the setting travels all the way from the client to
    // the pass, and every hop in between is somewhere it could quietly stop.
    const std::string source = "void Move(int &x) { }\n";

    CHECK(Published(DiagnosticsFor(source), "as-err-inout-on-primitive"));

    config::ServerConfig unsafeReferences;
    unsafeReferences.engine.allowUnsafeReferences = true;
    CHECK_FALSE(Published(DiagnosticsFor(source, unsafeReferences), "as-err-inout-on-primitive"));
}

TEST_CASE("Server - Publishes a diagnostic only an engine property switches on")
{
    // The other direction: silent at the engine's defaults, and reported once the host says its
    // engine forbids global variables.
    const std::string source = "int g_count = 0;\nvoid main() { }\n";

    CHECK_FALSE(Published(DiagnosticsFor(source), "as-err-global-vars-disallowed"));

    config::ServerConfig noGlobals;
    noGlobals.engine.disallowGlobalVars = true;
    CHECK(Published(DiagnosticsFor(source, noGlobals), "as-err-global-vars-disallowed"));
}

TEST_CASE("Server - Publishes the type conversion diagnostics")
{
    const std::string source =
        "class Money {}\n"
        "void main() { Money m = 1; }\n";

    CHECK(Published(DiagnosticsFor(source), "as-err-no-implicit-conversion"));
}

TEST_CASE("Server - A severity override reaches a restored diagnostic")
{
    // The only part of the configuration the restored codes had never exercised: nothing proved a
    // code that did not exist when the override plumbing was written could be retargeted by it.
    const std::string source = "void g_nothing;\n";

    config::ServerConfig serverConfig;
    serverConfig.diagnosticSeverities["as-err-void-variable"] = "hint";

    const std::string output = DiagnosticsFor(source, serverConfig);
    REQUIRE(Published(output, "as-err-void-variable"));

    // Hint is severity 4 in the protocol; the rule's own severity is Error, which is 1.
    // Hint is 4 on the wire. Before the severity mapping was fixed this arrived as 0, because a
    // cast between the two enumerations ran Hint off the end of the protocol's table.
    CHECK(PublishedFrames(output).find("\"severity\":4") != std::string::npos);
}

TEST_CASE("Server - An error is published as an error, not as a warning")
{
    // Regression, and the reason the Layer 4 coverage above was worth writing. lsp::
    // DiagnosticSeverity is a 0-based enum whose serializer maps its index onto the wire values
    // {1,2,3,4}; analysis::DiagnosticSeverity is numbered 1..4 to match those values directly.
    // Casting one to the other shifted every diagnostic by one, so every error this server
    // published had been arriving in the editor as a warning, and every warning as information.
    const std::string source = "void g_nothing;\n";

    const std::string frames = PublishedFrames(DiagnosticsFor(source));
    REQUIRE(frames.find("\"as-err-void-variable\"") != std::string::npos);
    CHECK(frames.find("\"severity\":1") != std::string::npos);
    CHECK(frames.find("\"severity\":2") == std::string::npos);
}

TEST_CASE("Server - A warning is published as a warning")
{
    const std::string source = "void main() { int unused = 1; }\n";

    const std::string frames = PublishedFrames(DiagnosticsFor(source));
    REQUIRE(frames.find("\"as-warn-unused-variable\"") != std::string::npos);
    CHECK(frames.find("\"severity\":2") != std::string::npos);
}

TEST_CASE("Server - An opened predefined stub is not judged as a script")
{
    // A stub's functions have no bodies by design. Recognising the file is what stands between the
    // user and one error per declaration - the real Sven Coop stub produces 3144 of them when it is
    // read as ordinary script. The file is named `as.predefined`, AngelScript's own convention,
    // which the configured `.as.predefined` suffix does not match on its own.
    const std::string stub =
        "class CBaseEntity\n"
        "{\n"
        "    void Spawn();\n"
        "    void Precache();\n"
        "    int TakeDamage(CBaseEntity@ attacker, float damage);\n"
        "}\n"
        "void ServerCommand(const string &in command);\n";

    WorkspaceFixture fixture;
    fixture.Write("as.predefined", stub);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("as.predefined"), stub));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string frames = PublishedFrames(stream.Output());
    CHECK(frames.find("\"as-err-missing-body\"") == std::string::npos);
    CHECK(frames.find("\"as-err-declaration-missing-body\"") == std::string::npos);

    // The same content under a name that is not a stub must still be reported, or the check above
    // would pass for the wrong reason - a server that simply never analysed the file would satisfy
    // it just as well.
    WorkspaceFixture asScript;
    asScript.Write("main.as", stub);

    test::ScriptedStream scriptStream;
    scriptStream.Push(InitializeMessage(asScript.RootUri()));
    scriptStream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    scriptStream.Push(DidOpenMessage(asScript.Uri("main.as"), stub));
    scriptStream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    RunScript(serverConfig, scriptStream);
    CHECK(PublishedFrames(scriptStream.Output()).find("\"as-err-missing-body\"") != std::string::npos);
}

// =====================================================================================
// Transport resilience.
//
// The framework writes a JSON-RPC error response for a malformed frame and then rethrows, and
// neither json::ParseError nor jsonrpc::ProtocolError derives from ConnectionError. Server::Run
// used to catch only the connection types, so those escaped main() and reached std::terminate:
// one stray byte from the client killed the process, and because terminate skips destructors the
// analysis and workspace threads were torn down while the state they read was still in use.
//
// These cases exist to keep that closed. Each pushes a bad frame *before* a valid initialize, so a
// server that dies on the bad one never answers the good one and the assertion fails.
// =====================================================================================

TEST_CASE("Server - Recovers from a malformed JSON frame and keeps serving")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    // Well-framed (the Content-Length is honest) but the body is truncated JSON, so the parser
    // fails after the frame has been fully consumed - the stream is still aligned on a boundary.
    stream.Push(R"({"jsonrpc":"2.0","id":1,"method":)");
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"hoverProvider\""));
}

TEST_CASE("Server - Recovers from a structurally invalid JSON-RPC message")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    // Valid JSON, invalid JSON-RPC: an empty batch, which the framework rejects with a
    // ProtocolError rather than a ParseError. Different exception, same former fatality.
    stream.Push(R"([])");
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("\"hoverProvider\""));
}

// =====================================================================================
// The call graph has to survive a re-analysis.
//
// AnalyzeDocument populated m_callGraph and then cleared it one line later: the ClearDocument call
// sat outside the `else` it was meant to belong to, so it ran on every pass whether or not there
// was a tree. Call hierarchy therefore went empty after the first edit to a file and stayed empty
// until it was reopened - silently, since nothing publishes the call graph as a diagnostic.
//
// This drives the debounced path deliberately: didChange schedules analysis on the background
// thread, and the pause below lets it finish before the query. Also the only didChange coverage in
// this harness - the ts_tree_edit path had none at all.
// =====================================================================================

TEST_CASE("Server - Keeps the call graph after a document is re-analysed")
{
    const std::string before = "void helper() {}\nvoid main() { helper(); }\n";
    const std::string after  = "void helper() {}\nvoid main() { helper(); helper(); }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", before);
    const std::string uri = fixture.Uri("main.as");

    // Hand-built rather than echoed back from prepareCallHierarchy: the outgoing-calls handler
    // reads the item straight off the request, so nothing here depends on parsing a prior reply.
    const std::string item =
        R"({"name":"main","kind":12,"uri":")" + uri + R"(",)"
        R"("range":{"start":{"line":1,"character":0},"end":{"line":1,"character":33}},)"
        R"("selectionRange":{"start":{"line":1,"character":5},"end":{"line":1,"character":9}}})";

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(uri, before));

    // Whole-document sync, which is the branch that drops the tree and reparses from scratch.
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{)"
                R"("uri":")" + uri + R"(","version":2},"contentChanges":[{"text":")" +
                JsonEscape(after) + R"("}]}})");

    // Runs on the message loop before the next frame is read, so the debounced analysis (200 ms)
    // has finished by the time the query below is handled. Not a race: the server is blocked here.
    stream.PushAction([]() { std::this_thread::sleep_for(std::chrono::milliseconds(900)); });

    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"callHierarchy/outgoingCalls","params":{"item":)" + item + R"(}})");
    stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // The call from main() to helper() must still be there after the re-analysis.
    CHECK(stream.OutputContains("\"helper\""));
}

// =====================================================================================
// Progress on the workspace scan.
//
// The scan reads and indexes every script and stub under every workspace folder before a single
// cross-file symbol resolves, and it used to do all of that silently - on a large workspace the
// server just appears to know nothing for a while, which reads as broken rather than busy.
//
// A server may only report progress against a token it created, and `window/workDoneProgress/create`
// exists only where the client advertised `window.workDoneProgress`. So both directions are pinned:
// a client that asks for it gets it, and one that does not is not sent notifications it has nowhere
// to put.
// =====================================================================================

namespace
{
    std::string InitializeWithProgress(const std::string &rootUri, bool workDoneProgress)
    {
        return R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
               R"("processId":null,"rootUri":")" + rootUri + R"(",)"
               R"("capabilities":{"window":{"workDoneProgress":)" +
               (workDoneProgress ? "true" : "false") + R"(}},)"
               R"("workspaceFolders":[{"uri":")" + rootUri + R"(","name":"fixture"}]}})";
    }
}

TEST_CASE("ServerHarness - Reports workspace scan progress when the client supports it")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    // The scan starts on `initialized`, not on `initialize` - without this notification there is no
    // scan to report on.
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // The scan runs on its own thread, and the server tears that thread down as soon as the scripted
    // input runs out - which, without this, happens before the thread is even scheduled. Waiting for
    // the "end" notification is what makes the assertions below about a finished scan rather than a
    // race. Bounded, so a scan that never finishes fails the test instead of hanging it.
    // A request after the notification, so the loop has demonstrably come back round and dispatched
    // `initialized` before the wait below begins. Without it the action fires while that
    // notification is still in flight and the scan has not been started yet.
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.Output().find("\"kind\":\"end\"") != std::string::npos)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    const std::string output = RunScript(serverConfig, stream);

    INFO(output);
    CHECK(output.find("window/workDoneProgress/create") != std::string::npos);
    CHECK(output.find("$/progress") != std::string::npos);
    CHECK(output.find("angelscript-workspace-scan-") != std::string::npos);
}

TEST_CASE("ServerHarness - Sends no progress to a client that did not ask for it")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/false));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    config::ServerConfig serverConfig;
    const std::string output = RunScript(serverConfig, stream);

    INFO(output);
    CHECK(output.find("$/progress") == std::string::npos);
    CHECK(output.find("window/workDoneProgress/create") == std::string::npos);
}

// =====================================================================================
// One file, several spellings, one document.
//
// The same file arrives written more than one way: VS Code sends `file:///e%3A/dir/f.as`, the
// workspace scan synthesises `file:///E:/dir/f.as` from the path it walked, and an `#include`
// resolves to a third. Keyed raw, those were three documents - an edit to one left the others
// stale, and the predefined loader had already needed a private map to work around exactly this.
//
// Server::DocumentKey is the one place that answers "which document is this", and
// m_clientUriByKey is what keeps diagnostics reaching the editor the user is actually looking at.
// =====================================================================================

TEST_CASE("Server - Two spellings of one URI are one document")
{
    const std::string source = "void Think() { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    const std::string plain = fixture.Uri("main.as");

    // The spelling a client actually sends on Windows: percent-encoded drive colon, lowercased
    // drive letter. PathToUri writes neither, which is what made these two different strings.
    std::string encoded = plain;
    const size_t colon = encoded.find(":/", std::string("file:///").size());
    if (colon != std::string::npos)
    {
        encoded.replace(colon, 1, "%3A");
        const size_t drive = std::string("file:///").size();
        encoded[drive] = static_cast<char>(std::tolower(static_cast<unsigned char>(encoded[drive])));
    }

    INFO("plain:   " << plain);
    INFO("encoded: " << encoded);

    // The two spellings only differ where there is a drive letter to encode. Elsewhere they are the
    // same string and there is nothing to test - so this skips rather than asserting, which is what
    // it used to do: a hard REQUIRE here failed the suite on Linux for a case that cannot arise
    // there. Found by running the suite in a container, which is the whole point of having one.
    if (plain == encoded)
    {
        MESSAGE("No drive letter in this path, so the two spellings cannot differ - skipped.");
        return;
    }

    // Opened one way and asked about the other. Keyed raw, the request would find no document and
    // the answer would be empty.
    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(encoded, source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":")" +
                plain + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("Think"));
}

TEST_CASE("Server - Diagnostics come back under the spelling the client sent")
{
    // The half that a key alone would have broken. Everything inside the server is keyed by the
    // canonical form; a diagnostic published under it is addressed to a document the client has
    // never heard of, so the user would see nothing at all.
    const std::string source = "void Think(  { }\n";  // deliberately malformed, to force one

    WorkspaceFixture fixture;
    fixture.Write("broken.as", source);

    std::string encoded = fixture.Uri("broken.as");
    const size_t colon = encoded.find(":/", std::string("file:///").size());

    // Same reason as the test above: there is no drive colon to percent-encode on a path that has
    // no drive letter, so there is no second spelling and nothing to check. Skipped, not failed.
    if (colon == std::string::npos)
    {
        MESSAGE("No drive letter in this path, so there is no second spelling - skipped.");
        return;
    }

    encoded.replace(colon, 1, "%3A");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(encoded, source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    INFO("expected the client's own spelling: " << encoded);
    CHECK(stream.OutputContains("publishDiagnostics"));
    CHECK(stream.OutputContains("%3A"));
}

TEST_CASE("Server - Editing a predefined stub re-diagnoses the open documents")
{
    // A stub is how a user tells this server about the types their host registers in C++, so the
    // diagnostics an edit to it changes are precisely the ones they are watching. The reload
    // happened - the stub was re-read - but the branch that did it skipped the flag that fans the
    // change out, so every open document kept being judged against the old stub until something
    // else happened to touch it.
    WorkspaceFixture fixture;

    // A document naming a type only the stub can supply.
    const std::string source = "void Think(HostThing@ thing) { }\n";
    fixture.Write("main.as", source);

    // The stub, initially declaring nothing of the sort.
    fixture.Write("engine.as.predefined", "class SomethingElse {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

    // Now the stub learns about it, and the client reports the change on disk.
    fixture.Write("engine.as.predefined", "class SomethingElse {}\nclass HostThing {}\n");
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[{"uri":")" +
                fixture.Uri("engine.as.predefined") + R"(","type":2}]}})");

    // The fan-out schedules analysis on the worker thread, which debounces for 200ms and drops
    // whatever is pending the moment shutdown is requested - correct in production, and the reason
    // this has to wait before asking for shutdown rather than racing it.
    // Two spacers before the wait. The reader runs a frame ahead of the message loop, so an action
    // registered immediately after a notification fires while that notification is still being
    // processed - the wait has to sit far enough behind to land after the fan-out has scheduled.
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.PushAction([] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enablePredefinedLoader = true;
    RunScript(serverConfig, stream);

    // Two publishDiagnostics for the document: the one from didOpen, and a second after the stub
    // changed. Without the fan-out there is only ever the first.
    const std::string needle = "publishDiagnostics";
    size_t count = 0;
    const std::string output = stream.Output();
    for (size_t at = output.find(needle); at != std::string::npos; at = output.find(needle, at + 1))
    {
        ++count;
    }
    INFO("publishDiagnostics notifications: " << count);
    CHECK(count >= 2);
}

// =====================================================================================
// workspace/didRenameFiles and didDeleteFiles.
//
// The editor knows about a rename before the filesystem watcher does, and it knows it as ONE
// operation rather than as whatever burst of create/delete events the platform produces. That is
// what makes the #include fixup possible at all: by the time a watcher reports a deletion and a
// creation, nothing connects the two.
// =====================================================================================

TEST_CASE("Server - Announces the file-operation capabilities")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.OutputContains("fileOperations"));
    CHECK(stream.OutputContains("didRename"));
    CHECK(stream.OutputContains("didDelete"));

    // The `will` variants are requests, and answering one blocks the rename in the editor until the
    // server replies. Nothing here needs to veto an operation, so they are not announced.
    CHECK_FALSE(stream.OutputContains("willRename"));
}

TEST_CASE("Server - Renaming an included file rewrites the #include that named it")
{
    WorkspaceFixture fixture;

    const std::string mainSource = "#include \"helper.as\"\nvoid Think() { }\n";
    fixture.Write("main.as", mainSource);
    fixture.Write("helper.as", "void Helped() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainSource));

    // A save, not just an open: didSave patches the include graph synchronously, whereas the graph
    // an open relies on is built by the background workspace scan - and the rewrite needs the edge
    // that says main.as includes helper.as. Same reason the watched-files test does this.
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"text":")" + JsonEscape(mainSource) + R"("}})");

    // The file is not moved on disk here, on purpose. The rewrite is computed from the graph edge
    // and the INCLUDING file's text, neither of which is the renamed file - and moving it would
    // have to be sequenced against the message loop, which the reader runs a frame ahead of.
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didRenameFiles","params":{"files":[{"oldUri":")" +
                fixture.Uri("helper.as") + R"(","newUri":")" + fixture.Uri("renamed.as") + R"("}]}})");

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // An applyEdit request carrying the new name. Sent rather than written: the change belongs on
    // the editor's undo stack beside the rename that caused it.
    CHECK(stream.OutputContains("workspace/applyEdit"));
    CHECK(stream.OutputContains("renamed.as"));
}

TEST_CASE("Server - Renaming a file nothing includes asks for no edit")
{
    // The guard against a rename storm: a directory rename reports every file in it, and an edit
    // per file that nobody references would be noise the user has to review and undo.
    WorkspaceFixture fixture;

    const std::string mainSource = "void Think() { }\n";
    fixture.Write("main.as", mainSource);
    fixture.Write("orphan.as", "void Alone() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainSource));
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didRenameFiles","params":{"files":[{"oldUri":")" +
                fixture.Uri("orphan.as") + R"(","newUri":")" + fixture.Uri("moved.as") + R"("}]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK_FALSE(stream.OutputContains("workspace/applyEdit"));
}

TEST_CASE("Server - Deleting a file drops its symbols from the workspace")
{
    WorkspaceFixture fixture;

    const std::string mainSource = "#include \"helper.as\"\nvoid Think() { }\n";
    fixture.Write("main.as", mainSource);
    fixture.Write("helper.as", "void UniquelyNamedHelper() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainSource));
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("},"text":")" + JsonEscape(mainSource) + R"("}})");

    // Present before.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"UniquelyNamedHelper"}})");

    stream.PushAction([dir = fixture.dir]()
    {
        std::error_code ec;
        std::filesystem::remove(dir / "helper.as", ec);
    });
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didDeleteFiles","params":{"files":[{"uri":")" +
                fixture.Uri("helper.as") + R"("}]}})");

    // And gone after.
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"workspace/symbol","params":{"query":"UniquelyNamedHelper"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // Asserted per reply rather than over the whole transcript, the way the watched-files test
    // does: the server also writes log notifications that quote the name.
    CHECK(stream.ResponseFor(2).find("UniquelyNamedHelper") != std::string::npos);
    CHECK(stream.ResponseFor(3).find("UniquelyNamedHelper") == std::string::npos);
}

// =====================================================================================
// Pull diagnostics - textDocument/diagnostic and workspace/diagnostic.
//
// The push model has one structural hole: the server decides when to send, so a client that was
// not listening yet, or that wants diagnostics for a file it is about to show, has no way to ask.
// LSP 3.17 added the request; this server now answers it from the cache the analysis thread fills,
// because running the analyzer on the message loop would race that thread over the symbol table.
//
// The point these tests defend is that pull and push cannot disagree. Both go through
// ToProtocolDiagnostics, so a diagnostic that reaches one reaches the other.
// =====================================================================================

TEST_CASE("Server - Announces the pull-diagnostic capabilities")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string initializeReply = stream.ResponseFor(1);
    INFO(initializeReply);
    CHECK(initializeReply.find("diagnosticProvider") != std::string::npos);

    // An #include changes the diagnostics of every file including it, which is the case the flag
    // exists for - a client that reads it false will not re-pull the includers.
    CHECK(initializeReply.find("\"interFileDependencies\":true") != std::string::npos);
    CHECK(initializeReply.find("\"workspaceDiagnostics\":true") != std::string::npos);
}

TEST_CASE("Server - A pulled diagnostic carries what the pushed one carried")
{
    // Same document, same finding, both routes. If these two ever diverge the server is telling two
    // different stories about one file depending on which way the client asked.
    WorkspaceFixture fixture;

    const std::string source = "void Main()\n{\n    UndefinedThingy();\n}\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

    // The analyzer debounces for 200ms on its own thread and the cache is filled by the publish
    // that follows. Two spacers first: the reader runs a frame ahead of the message loop, so an
    // action registered right after a notification fires while that notification is still being
    // processed.
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("publishDiagnostics"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string pulled = stream.ResponseFor(2);
    INFO(pulled);

    CHECK(pulled.find("\"kind\":\"full\"") != std::string::npos);
    CHECK(pulled.find("resultId") != std::string::npos);

    // The finding itself, and it must be the same code the push route carried.
    CHECK(pulled.find("UndefinedThingy") != std::string::npos);
    CHECK(Published(stream.Output(), "as-err-undefined-identifier"));
    CHECK(pulled.find("as-err-undefined-identifier") != std::string::npos);
}

TEST_CASE("Server - A second pull of an unedited document is answered unchanged")
{
    // What the pull model is for. The client echoes the result id back and an untouched file costs
    // one string instead of its whole diagnostic list.
    //
    // Run twice rather than once. The follow-up request has to carry the id the server chose, and
    // that id cannot be scripted in advance - but neither can it be read mid-session: PushAction
    // runs inside the message loop's own read(), so an action waiting for a reply is waiting for
    // the thread it is blocking. So the first run learns the id and the second, over an identical
    // script against a fresh server, sends it back. Identical input, identical id; if that ever
    // stops holding, this fails loudly rather than quietly testing nothing.
    const std::string source = "void Main()\n{\n    UndefinedThingy();\n}\n";

    const auto pullOnce = [&source](const std::string &previousResultId)
    {
        WorkspaceFixture fixture;
        fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeMessage(fixture.RootUri()));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

        stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
        stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
        stream.PushAction([&stream]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains("publishDiagnostics"))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        std::string request = R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                              fixture.Uri("main.as") + R"("})";
        if (!previousResultId.empty())
            request += R"(,"previousResultId":")" + previousResultId + R"(")";
        request += "}}";

        stream.Push(request);
        stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);
        return stream.ResponseFor(2);
    };

    const std::string first = pullOnce("");
    INFO("first: " << first);
    REQUIRE(first.find("\"kind\":\"full\"") != std::string::npos);

    const size_t at = first.find("\"resultId\":\"");
    REQUIRE(at != std::string::npos);
    const size_t idStart = at + 12;
    const size_t idEnd = first.find('"', idStart);
    REQUIRE(idEnd != std::string::npos);
    const std::string resultId = first.substr(idStart, idEnd - idStart);
    REQUIRE_FALSE(resultId.empty());

    const std::string second = pullOnce(resultId);
    INFO("second: " << second);

    CHECK(second.find("\"kind\":\"unchanged\"") != std::string::npos);

    // Unchanged means unchanged: the items are not resent.
    CHECK(second.find("UndefinedThingy") == std::string::npos);
}

TEST_CASE("Server - Pulling a document the analyzer has not reached asks the client to retry")
{
    // The interesting case, and the one where the easy answer is wrong. An empty full report is a
    // positive claim that the file is clean. The server has not looked at this file, so it says so
    // - ServerCancelled with retriggerRequest, which is the protocol's way of "ask me again".
    WorkspaceFixture fixture;
    fixture.Write("never-opened.as", "void Main() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                fixture.Uri("never-opened.as") + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);

    CHECK(reply.find("\"error\"") != std::string::npos);
    CHECK(reply.find("-32802") != std::string::npos);
    CHECK(reply.find("\"retriggerRequest\":true") != std::string::npos);

    // And emphatically not a clean bill of health.
    CHECK(reply.find("\"kind\":\"full\"") == std::string::npos);
}

TEST_CASE("Server - workspace/diagnostic reports the documents already analysed")
{
    WorkspaceFixture fixture;

    const std::string source = "void Main()\n{\n    UndefinedThingy();\n}\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("publishDiagnostics"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/diagnostic","params":{"previousResultIds":[]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);

    CHECK(reply.find("\"items\"") != std::string::npos);
    CHECK(reply.find("main.as") != std::string::npos);
    CHECK(reply.find("as-err-undefined-identifier") != std::string::npos);
}

// =====================================================================================
// The pull-diagnostic kill switch.
//
// Every other capability this server offers has one, and a flag that is declared but does not
// actually switch anything off is worse than no flag: the setting promises a control that does
// nothing. So both halves are asserted - the capability disappears from initialize, and the
// requests stop being answered.
// =====================================================================================

TEST_CASE("Server - The pull-diagnostic capability disappears when the feature is off")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enablePullDiagnostics = false;
    RunScript(serverConfig, stream);

    const std::string initializeReply = stream.ResponseFor(1);
    INFO(initializeReply);
    CHECK(initializeReply.find("diagnosticProvider") == std::string::npos);
}

TEST_CASE("Server - A pull request is refused when the feature is off")
{
    // MethodNotFound rather than an empty report: an empty report is a positive claim that the
    // document is clean, and a switched-off feature has no opinion about the document at all.
    WorkspaceFixture fixture;

    const std::string source = "void Main()\n{\n    UndefinedThingy();\n}\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enablePullDiagnostics = false;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") != std::string::npos);
    CHECK(reply.find("-32601") != std::string::npos);
    CHECK(reply.find("\"kind\":\"full\"") == std::string::npos);
}

TEST_CASE("Server - Push diagnostics keep working with pull switched off")
{
    // The two are independent, which is the reason turning pull off is safe: a client that never
    // pulled loses nothing. If this ever fails, the kill switch took the notifications with it.
    WorkspaceFixture fixture;

    const std::string source = "void Main()\n{\n    UndefinedThingy();\n}\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("publishDiagnostics"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enablePullDiagnostics = false;
    RunScript(serverConfig, stream);

    CHECK(Published(stream.Output(), "as-err-undefined-identifier"));
}

TEST_CASE("Server - workspace/diagnostic answers empty rather than failing when off")
{
    // Unlike the document request, this one is answered. A workspace report listing no documents
    // is a truthful statement - the server is reporting on nothing - where an error would leave a
    // polling client retrying a request that will never succeed.
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void Main() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/diagnostic","params":{"previousResultIds":[]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.features.enablePullDiagnostics = false;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("\"items\":[]") != std::string::npos);
}


// =====================================================================================
// Three protocol messages that were in the framework's ClientToServer list and unanswered.
//
// The audit that found them compared every ClientToServer message the generated messages.h
// declares against the handlers Server.cpp registers. Most of what it turned up is deliberate -
// notebook documents, colour pickers, the `will` file operations that block the editor - but these
// three were absent for no reason but that nobody had written them.
// =====================================================================================

TEST_CASE("Server - Announces didCreate alongside didRename and didDelete")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(1);
    INFO(reply);
    CHECK(reply.find("didCreate") != std::string::npos);
    CHECK(reply.find("didRename") != std::string::npos);
    CHECK(reply.find("didDelete") != std::string::npos);
}

TEST_CASE("Server - The workspace scan announces itself as cancellable")
{
    // It was announced as `cancellable: false` while nothing handled the cancel notification, which
    // is the honest pairing. Now that one exists, the flag has to say so - a client will not offer
    // the button otherwise.
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    // With the progress capability declared: the server reports nothing at all to a client that
    // did not advertise window.workDoneProgress, so without this the assertion below would be
    // about a notification the server was right not to send.
    stream.Push(InitializeWithProgress(fixture.RootUri(), true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("\"kind\":\"end\""))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    const std::string output = RunScript(serverConfig, stream);

    INFO(output);
    CHECK(output.find("\"cancellable\":true") != std::string::npos);
}

TEST_CASE("Server - Survives a cancel naming a progress token it never issued")
{
    // A client may run several progress operations at once and cancel any of them. Cancelling on
    // the notification alone rather than on the token would have stopped this server's scan
    // because something entirely unrelated was dismissed.
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"window/workDoneProgress/cancel","params":{"token":"someone-elses-token"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - Accepts $/setTrace without complaint")
{
    // Unhandled, this was silently dropped: a notification gets no error reply, so a client asking
    // for verbose logging simply did not get it and had no way to find out.
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"verbose"}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/setTrace","params":{"value":"off"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - A created file that something includes becomes visible")
{
    // The editor knows about the file before any watcher tick does. Without this the `#include`
    // naming it stayed unresolved until something else happened to trigger a rescan.
    WorkspaceFixture fixture;

    const std::string mainSource = "#include \"helper.as\"\nvoid Think() { }\n";
    fixture.Write("main.as", mainSource);
    fixture.Write("helper.as", "void PlaceholderHelper() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainSource));

    // The editor creates the file, then reports it.
    stream.PushAction([dir = fixture.dir]()
    {
        std::ofstream out(dir / "helper.as", std::ios::binary);
        out << "void UniquelyNamedNewcomer() { }\n";
    });
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didCreateFiles","params":{"files":[{"uri":")" +
                fixture.Uri("helper.as") + R"("}]}})");

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"UniquelyNamedNewcomer"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("UniquelyNamedNewcomer") != std::string::npos);
}

TEST_CASE("Server - A created file nothing includes is not indexed")
{
    // The guard against a scaffolder. A template that writes forty files would otherwise cost forty
    // parses of code nobody has referenced yet.
    WorkspaceFixture fixture;

    const std::string mainSource = "void Think() { }\n";
    fixture.Write("main.as", mainSource);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), mainSource));

    stream.PushAction([dir = fixture.dir]()
    {
        std::ofstream out(dir / "unreferenced.as", std::ios::binary);
        out << "void NobodyAsksForThis() { }\n";
    });
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didCreateFiles","params":{"files":[{"uri":")" +
                fixture.Uri("unreferenced.as") + R"("}]}})");

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/symbol","params":{"query":"NobodyAsksForThis"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(stream.ResponseFor(2).find("NobodyAsksForThis") == std::string::npos);
}


// =====================================================================================
// The three resolve round-trips, and multi-range formatting.
//
// This server produces document links, inlay hints and workspace symbols complete - nothing is
// deferred - so the resolve handlers hand back what they were given. They exist because a client
// that insists on the round-trip otherwise gets MethodNotFound for a capability it was told about,
// and "already complete" is a better answer than an error.
// =====================================================================================

TEST_CASE("Server - Announces the three resolve providers it now answers")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(1);
    INFO(reply);

    // documentLinkProvider announced resolveProvider:false while nothing answered the request,
    // which was the honest pairing. All three say true now.
    CHECK(reply.find("\"documentLinkProvider\":{\"resolveProvider\":true}") != std::string::npos);
    CHECK(reply.find("\"inlayHintProvider\":{\"resolveProvider\":true}") != std::string::npos);
    CHECK(reply.find("\"workspaceSymbolProvider\":{\"resolveProvider\":true}") != std::string::npos);
}

TEST_CASE("Server - documentLink/resolve answers rather than failing")
{
    WorkspaceFixture fixture;
    fixture.Write("helper.as", "void Helped() { }\n");

    const std::string source = "#include \"helper.as\"\nvoid main() { }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"documentLink/resolve","params":)"
                R"({"range":{"start":{"line":0,"character":10},"end":{"line":0,"character":20}},)"
                R"("target":")" + fixture.Uri("helper.as") + R"("}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("helper.as") != std::string::npos);
}

TEST_CASE("Server - inlayHint/resolve answers rather than failing")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"inlayHint/resolve","params":)"
                R"({"position":{"line":0,"character":0},"label":"count:"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("count:") != std::string::npos);
}

TEST_CASE("Server - workspaceSymbol/resolve answers rather than failing")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspaceSymbol/resolve","params":)"
                R"({"name":"AlreadyComplete","kind":12,"location":{"uri":")" + fixture.Uri("main.as") +
                R"(","range":{"start":{"line":0,"character":0},"end":{"line":0,"character":5}}}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("AlreadyComplete") != std::string::npos);
}

TEST_CASE("Server - rangesFormatting formats every range it is given")
{
    const std::string source =
        "void  a( ) { int   x=1; }\n"
        "void  b( ) { int   y=2; }\n"
        "void  c( ) { int   z=3; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

    // The first and third lines, skipping the middle - the shape rangesFormatting exists for, and
    // the one a single range cannot express.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rangesFormatting","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},)"
                R"("ranges":[{"start":{"line":0,"character":0},"end":{"line":0,"character":25}},)"
                R"({"start":{"line":2,"character":0},"end":{"line":2,"character":25}}],)"
                R"("options":{"tabSize":4,"insertSpaces":true}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("newText") != std::string::npos);
}

TEST_CASE("Server - rangesFormatting with no ranges is an empty edit, not an error")
{
    const std::string source = "void a() { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/rangesFormatting","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"ranges":[],)"
                R"("options":{"tabSize":4,"insertSpaces":true}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
}


// =====================================================================================
// willSave, willSaveWaitUntil and executeCommand.
//
// The save pair is where a language server can do real damage: willSaveWaitUntil returns edits the
// editor applies to the user's file, so the question is not whether it can format but when it
// should be allowed to. Answer: only a manual save, and only when asked.
// =====================================================================================

TEST_CASE("Server - Announces the save hooks and its one command")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(1);
    INFO(reply);
    CHECK(reply.find("\"willSave\":true") != std::string::npos);
    CHECK(reply.find("\"willSaveWaitUntil\":true") != std::string::npos);
    CHECK(reply.find("angelscript.rescanWorkspace") != std::string::npos);
}

TEST_CASE("Server - A manual save formats nothing unless format-on-save was asked for")
{
    // The default, and the one that matters most. The editor has its own format-on-save setting; a
    // server that reformats regardless would override a choice made somewhere else, on a file the
    // user was only trying to save.
    const std::string source = "void  a( ) { int   x=1; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/willSaveWaitUntil","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"reason":1}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("newText") == std::string::npos);
}

TEST_CASE("Server - A manual save formats when format-on-save is on")
{
    const std::string source = "void  a( ) { int   x=1; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    // reason 1 is Manual.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/willSaveWaitUntil","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"reason":1}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.format.formatOnSave = true;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("newText") != std::string::npos);
}

TEST_CASE("Server - An autosave never formats, even with format-on-save on")
{
    // reason 2 is AfterDelay: the autosave timer, which fires while the user is still typing.
    // Rewriting the file underneath them is not something the setting above is allowed to buy.
    const std::string source = "void  a( ) { int   x=1; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/willSaveWaitUntil","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"reason":2}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.format.formatOnSave = true;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("newText") == std::string::npos);
}

TEST_CASE("Server - A focus-change save never formats either")
{
    // reason 3 is FocusOut. Same argument as the autosave: the user did not ask to save, so they
    // certainly did not ask to have the file rewritten.
    const std::string source = "void  a( ) { int   x=1; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/willSaveWaitUntil","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"reason":3}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.format.formatOnSave = true;
    RunScript(serverConfig, stream);

    CHECK(stream.ResponseFor(2).find("newText") == std::string::npos);
}

TEST_CASE("Server - willSave is consumed rather than dropped")
{
    const std::string source = "void a() { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/willSave","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},"reason":1}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

TEST_CASE("Server - executeCommand runs the rescan and refuses anything else")
{
    WorkspaceFixture fixture;
    fixture.Write("main.as", "void main() {}\n");

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/executeCommand","params":{"command":"angelscript.rescanWorkspace"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"workspace/executeCommand","params":{"command":"angelscript.notARealCommand"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string known = stream.ResponseFor(2);
    INFO(known);
    CHECK(known.find("\"error\"") == std::string::npos);

    // An unknown command is refused rather than silently doing nothing: a client that asked for
    // something this server does not have should hear so.
    const std::string unknown = stream.ResponseFor(3);
    INFO(unknown);
    CHECK(unknown.find("\"error\"") != std::string::npos);
    CHECK(unknown.find("notARealCommand") != std::string::npos);
}


// =====================================================================================
// The last four ClientToServer messages.
//
// Three of them do real work; the fourth, $/cancelRequest, deliberately does none, and the reason
// is measured rather than asserted - see the handler.
// =====================================================================================

TEST_CASE("Server - Announces moniker and the virtual-document scheme, but not inline completion")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(1);
    INFO(reply);

    CHECK(reply.find("\"monikerProvider\":true") != std::string::npos);
    CHECK(reply.find("angelscript-predefined") != std::string::npos);

    // Registered but NOT announced, and that pairing is the point: the server answers the request
    // if a client sends it anyway, while telling no client it has ghost text to offer.
    CHECK(reply.find("inlineCompletionProvider") == std::string::npos);
}

TEST_CASE("Server - A moniker names the symbol under the cursor")
{
    const std::string source =
        "class Entity\n"
        "{\n"
        "    void Think() { }\n"
        "}\n"
        "void main() { Entity e; e.Think(); }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    // On `Think` at the call site.
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/moniker","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},)"
                R"("position":{"line":4,"character":26}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"scheme\":\"angelscript\"") != std::string::npos);
    CHECK(reply.find("Entity::Think") != std::string::npos);

    // Project rather than Global: nothing here can promise this identifier is unique across every
    // AngelScript project, and claiming Global would have an indexer merge unrelated symbols.
    CHECK(reply.find("\"unique\":\"project\"") != std::string::npos);
}

TEST_CASE("Server - A moniker on empty space answers null rather than inventing one")
{
    const std::string source = "void main() { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/moniker","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},)"
                R"("position":{"line":0,"character":13}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
    CHECK(reply.find("\"scheme\"") == std::string::npos);
}

TEST_CASE("Server - inlineCompletion answers empty rather than failing")
{
    const std::string source = "void main() { }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/inlineCompletion","params":)"
                R"({"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("},)"
                R"("position":{"line":0,"character":13},)"
                R"("context":{"triggerKind":1}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") == std::string::npos);
}

TEST_CASE("Server - textDocumentContent refuses a scheme it does not serve")
{
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/textDocumentContent","params":{"uri":"file:///etc/passwd"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") != std::string::npos);
}

TEST_CASE("Server - textDocumentContent refuses a stub it never loaded")
{
    // The guard that keeps this from becoming a file server: only a stub this server actually
    // loaded is readable, however the URI is spelled.
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/textDocumentContent","params":{"uri":"angelscript-predefined:C:/nowhere/nothing.as.predefined"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string reply = stream.ResponseFor(2);
    INFO(reply);
    CHECK(reply.find("\"error\"") != std::string::npos);
    CHECK(reply.find("No predefined stub is loaded") != std::string::npos);
}

TEST_CASE("Server - $/cancelRequest is consumed rather than dropped")
{
    // It does nothing, and the handler says why: with synchronous dispatch the cancel is always
    // read after the request it names has finished. Registered so the notification is consumed.
    WorkspaceFixture fixture;

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":42}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    CHECK_NOTHROW(RunScript(serverConfig, stream));
}

// =====================================================================================
// `#define` in a predefined stub, driving what the preprocessor keeps.
//
// The two cases are the same document under two stubs, and they have to be read as a pair: the
// second alone would pass against a server that had crashed before publishing anything, so it
// asserts that diagnostics were published *and* that none of them came from inside the `#if`.
// =====================================================================================

namespace
{
    /** @brief Runs one `#if FOO` document under a stub and returns everything the server said. */
    std::string RunUnderStub(const std::string &stubText)
    {
        WorkspaceFixture fixture;
        fixture.Write("engine.as.predefined", stubText);

        const std::string source =
            "#if FOO\n"
            "void Main()\n"
            "{\n"
            "    UndefinedThingy();\n"
            "}\n"
            "#endif\n";
        fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

        // The stub is read by the workspace scan, so opening the document before the scan reports
        // "end" would analyse it against a server that has not seen the `#define` yet - and the
        // test would then be measuring the race rather than the feature.
        stream.PushAction([&stream]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains("\"kind\":\"end\""))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));

        // And analysis runs on its own thread, so shutting down straight after didOpen would end
        // the session before anything was published. Both of these tests passed that way once -
        // the one expecting silence passed because there was silence about everything.
        stream.PushAction([&stream]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains("publishDiagnostics"))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        return stream.Output();
    }
}

TEST_CASE("Server - A stub's #define makes the #if block it names live")
{
    const std::string output = RunUnderStub("#define FOO\n");
    INFO(PublishedFrames(output));

    CHECK(Published(output, "as-err-undefined-identifier"));
}

TEST_CASE("Server - Without that #define the same block is excluded and says nothing")
{
    const std::string output = RunUnderStub("// This stub defines no words at all.\n");

    // Published at all - otherwise the silence below proves nothing.
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    const std::string frames = PublishedFrames(output);
    INFO(frames);

    CHECK_FALSE(Published(output, "as-err-undefined-identifier"));
    CHECK(frames.find("\"as-err-") == std::string::npos);
}

TEST_CASE("Server - A #define in a script is reported, and the same one in a stub is not")
{
    // The pair matters more than either half: `#define` is a syntax error in a .as and this
    // server's own syntax in a .as.predefined, so a rule that could not tell them apart would
    // either miss the error or tell the user off for configuring the server as documented.
    WorkspaceFixture fixture;
    fixture.Write("engine.as.predefined", "#define FOO\n");

    const std::string source = "#define LOCAL\nvoid main() { }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("\"kind\":\"end\""))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("publishDiagnostics"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string frames = PublishedFrames(stream.Output());
    INFO(frames);

    CHECK(Published(stream.Output(), "as-warn-unsupported-directive"));

    // And it is the script's line that was reported, not the stub's. The stub is the only file
    // here whose `#define` is legitimate, and it sits on line 0 of its own document.
    CHECK(frames.find("engine.as.predefined") == std::string::npos);
}

// =====================================================================================
// Switching engine profile has to unload the profile that was left.
//
// The profiles load as synthetic documents - `builtin:///profiles/<name>.as.predefined` - claimed
// through the same ClaimPredefinedFile path as a stub on disk. didChangeConfiguration notices the
// profile changed and sets shouldRescan, but a rescan only ever *adds*: ClaimPredefinedFile refuses
// a URI it has already seen, and nothing releases the one that is no longer wanted.
//
// `Vector` is declared only by the SvenCoop profile, so a script naming it after a move to Urho3D
// should stop resolving. Still resolving means the old profile was never unloaded.
// =====================================================================================

TEST_CASE("Server - Switching engine profile forgets the profile that was left")
{
    const std::string source = "void main() { Vector v; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", source);

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle, size_t times)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::string output = stream.Output();
            size_t count = 0;
            for (size_t at = output.find(needle); at != std::string::npos;
                 at = output.find(needle, at + needle.size()))
            {
                ++count;
            }
            if (count >= times)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // The first scan loads the SvenCoop profile named in the config at the bottom.
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\"", 1); });

    // Move to a profile that has never heard of Vector.
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":)"
                R"({"settings":{"angelscript":{"engineProfile":"urho3d"}}}})");

    // The rescan runs its own progress cycle, so wait for a second one to finish.
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\"", 2); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { waitFor(stream, "publishDiagnostics", 1); });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.engineProfile = "svencoop";
    RunScript(serverConfig, stream);

    const std::string frames = PublishedFrames(stream.Output());
    INFO(frames);

    // Published at all - otherwise the assertion below would pass on a server that said nothing.
    REQUIRE(stream.Output().find("publishDiagnostics") != std::string::npos);

    CHECK(Published(stream.Output(), "as-err-unresolved-type"));
}

// =====================================================================================
// One active stub, the rest ignored.
//
// The three cases are a set and only mean something together: with a selection the other stub's
// declarations must be gone, without one they must both be there, and the engine profile has to
// survive either way. Testing only the first would pass against a server that had simply stopped
// loading stubs at all.
// =====================================================================================

namespace
{
    /** @brief A workspace with two stubs, each declaring a type the other does not. */
    struct TwoStubFixture
    {
        WorkspaceFixture fixture;

        TwoStubFixture()
        {
            fixture.Write("host_a.as.predefined", "class TypeFromA { void Poke(); }\n");
            fixture.Write("host_b.as.predefined", "class TypeFromB { void Poke(); }\n");
        }

        std::string Stub(const char *name) const { return (fixture.dir / name).generic_string(); }
    };

    /** @brief Runs one document against this workspace and returns everything the server said. */
    std::string RunWithActiveStub(const TwoStubFixture &two,
                                  const std::string &source,
                                  const std::string &activeStub)
    {
        const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains(needle))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        two.fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

        stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), source));
        stream.PushAction([&]() { waitFor(stream, "publishDiagnostics"); });
        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        serverConfig.activePredefined = activeStub;

        // Not the default "none". The analyzer stays silent about a type it cannot see the world
        // of, so with an empty symbol table every one of these assertions would pass by vacuity.
        serverConfig.engineProfile = "standard";

        RunScript(serverConfig, stream);
        return stream.Output();
    }
}

TEST_CASE("Server - An active stub is the only one the workspace scan loads")
{
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { TypeFromB b; }\n", two.Stub("host_a.as.predefined"));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    // B was not selected, so nothing it declares exists.
    CHECK(Published(output, "as-err-unresolved-type"));
}

TEST_CASE("Server - The active stub itself still resolves")
{
    // The other half of the same run. Without this, a server that loaded no stub at all would pass
    // the test above.
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { TypeFromA a; }\n", two.Stub("host_a.as.predefined"));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-unresolved-type"));
}

TEST_CASE("Server - With no selection the scan picks one stub rather than merging them")
{
    // The default used to load every stub it found and warn about the duplicate declarations that
    // followed - a default that was wrong and said so. Now the safe choice is made first: the first
    // stub in path order, so the same one on every machine, and the user is told which.
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { TypeFromA a; TypeFromB b; }\n", /*activeStub=*/"");

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    // host_a sorts first, so its type resolves and host_b's does not.
    CHECK(Published(output, "as-err-unresolved-type"));
    CHECK(PublishedFrames(output).find("TypeFromB") != std::string::npos);
    CHECK(PublishedFrames(output).find("TypeFromA") == std::string::npos);

    // Said once, naming the winner and the way out. Not a warning any more: nothing went wrong.
    CHECK(output.find("using host_a.as.predefined of 2 predefined stubs found") != std::string::npos);
}

TEST_CASE("Server - Asking for all of them brings the merge back")
{
    // The old default, now something a workspace has to ask for. A host whose API is split across
    // two stubs needs exactly this, and it has to stay reachable.
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { TypeFromA a; TypeFromB b; }\n", /*activeStub=*/"all");

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-unresolved-type"));
    CHECK(output.find("2 predefined stubs loaded together") != std::string::npos);
}

TEST_CASE("Server - A lone stub is loaded without a word about it")
{
    // Nothing to choose between is nothing to say. The message above exists to explain a choice the
    // server made; a workspace with one stub was never in doubt.
    WorkspaceFixture fixture;
    fixture.Write("only.as.predefined", "class TypeFromA { }\n");
    fixture.Write("main.as", "void main() { TypeFromA a; }\n");

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // The scan has to have finished before the document is opened, or the stub is simply not loaded
    // yet and this would be measuring the race rather than the rule.
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), "void main() { TypeFromA a; }\n"));
    stream.PushAction([&]() { waitFor(stream, "publishDiagnostics"); });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    const std::string output = RunScript(serverConfig, stream);

    INFO(PublishedFrames(output));
    CHECK_FALSE(Published(output, "as-err-unresolved-type"));
    CHECK(output.find("predefined stubs found") == std::string::npos);
    CHECK(output.find("predefined stubs loaded together") == std::string::npos);
}

TEST_CASE("Server - The engine profile survives a stub selection")
{
    // A host stub describes the host's API, not the standard library. If selecting one dropped the
    // profile as well, every workspace that chose a stub would lose `array` and `string`.
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { array<int> xs; }\n", two.Stub("host_a.as.predefined"));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-unresolved-type"));
}

TEST_CASE("Server - A selection naming a file that is not there is reported, loudly")
{
    // The failure this prevents is silent: a mistyped path would load no stub, every host type
    // would stop resolving, and nothing on screen would say why.
    TwoStubFixture two;

    const std::string output = RunWithActiveStub(
        two, "void main() { }\n", two.Stub("host_that_does_not_exist.as.predefined"));

    CHECK(output.find("selected predefined stub was not found") != std::string::npos);
}

TEST_CASE("Server - The selection matches the file, not the spelling of its path")
{
    // The setting arrives with whatever spelling the client used and the walk produces the
    // filesystem's own. On Windows those differ in case for the same file, and this project
    // already carries m_clientUriByKey because that difference bit it once. Comparing the two as
    // text would silently select nothing, which looks exactly like a mistyped path.
    TwoStubFixture two;

    std::string shouted = two.Stub("host_a.as.predefined");
    for (char &c : shouted)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Whether the shouted path is the same file is the filesystem's business, not the platform's,
    // and this test used to decide it with `#if defined(_WIN32)`. That is wrong on macOS, whose
    // default filesystem is case-insensitive: weakly_canonical resolves the upper-cased path back
    // to the real on-disk spelling, the selection succeeds, and the test failed expecting a
    // complaint that correctly never came. Windows and macOS agree here; Linux does not. So ask
    // the filesystem instead of guessing from the operating system.
    std::error_code ec;
    const bool caseInsensitive = std::filesystem::exists(std::filesystem::path(shouted), ec) && !ec;

    const std::string output = RunWithActiveStub(two, "void main() { TypeFromA a; }\n", shouted);

    INFO(PublishedFrames(output));
    INFO("case-insensitive filesystem: " << caseInsensitive);
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    if (caseInsensitive)
    {
        // Same file, louder spelling: it resolves, and nothing complains about a missing selection.
        CHECK_FALSE(Published(output, "as-err-unresolved-type"));
        CHECK(output.find("selected predefined stub was not found") == std::string::npos);
    }
    else
    {
        // Here the case really is part of the name, so this is a different file and saying so is
        // the correct answer - which is the half that matters most: a selection naming a file the
        // scan never saw must be loud, or every host type stops resolving with nothing to explain it.
        CHECK(output.find("selected predefined stub was not found") != std::string::npos);
    }
}

TEST_CASE("Server - A pull answer is never about text the analyzer has not seen")
{
    // The bug this pins, reported from real use: typing the `;` that completes a statement left the
    // "missing ';'" error on screen until another keystroke. The editor renders push and pull as
    // two separate diagnostic collections, so the corrected push answer and the stale pull answer
    // sat side by side and only the second looked wrong.
    //
    // The cache stored what had been computed but not what it had been computed FROM, so the
    // handler could tell it had an answer and not whether that answer was still about this text.
    // After the first analysis it served the previous one forever.
    const std::string broken = "void main() { float f }\n";
    const std::string fixed   = "void main() { float f; }\n";

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    WorkspaceFixture fixture;
    fixture.Write("main.as", broken);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), broken));

    // Let the broken text be analysed, so the cache holds a real answer to go stale.
    stream.PushAction([&]() { waitFor(stream, "as-syntax-error"); });

    // The fix arrives, and the client pulls immediately - before the debounced analysis can run.
    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"(","version":2},"contentChanges":[{"text":")" +
                JsonEscape(fixed) + R"("}]}})");
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string pulled = stream.ResponseFor(2);
    INFO(pulled);

    // Either the analyzer had already caught up and the answer is clean, or it had not and the
    // server said so. What it must never do is hand back the previous document's findings.
    const bool refused = pulled.find("\"code\":-32802") != std::string::npos ||
                         pulled.find("retriggerRequest") != std::string::npos;
    const bool clean = pulled.find("as-syntax-error") == std::string::npos;

    CHECK((refused || clean));
}

// =====================================================================================
// Typing has to reach the client without a save.
//
// The refusal above is only half an answer: it carries retriggerRequest, so the client asks again
// straight away, and it queues the document so there is something to answer with. Queueing bumped
// the analysis revision, and the revision is what the 200ms debounce watches - so an editor
// polling faster than that pushed the deadline out on every ask and the analysis never ran. No
// notification went out, no pull was ever answered, and nothing on screen changed while the user
// typed. Saving looked like the cure because a save analyses on the message loop and never touches
// that queue at all, which is exactly how it was reported: "the changes only appear when I save".
//
// Scripted the way the client behaves - one edit, then a run of pulls closer together than the
// debounce - and asserted on what the user could actually see: the edit's own diagnostic published
// while the polling was still going on, and a pull answered rather than deferred again.
// =====================================================================================

TEST_CASE("Server - An edit reaches the client while a polling editor keeps asking")
{
    const std::string opened = "void main() { }\n";
    const std::string typed  = "void main() { int justTyped = 1; }\n";

    WorkspaceFixture fixture;
    fixture.Write("main.as", opened);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), opened));

    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"(","version":2},"contentChanges":[{"text":")" +
                JsonEscape(typed) + R"("}]}})");

    // Twenty polls at 50ms: about a second of a client asking, every one of them well inside the
    // quiet period the edit opened. The sleeps are the point of the test - consumed back to back
    // the whole burst would land in the same instant and never reach the deadline it has to cross.
    constexpr int k_firstPollId = 100;
    constexpr int k_polls = 20;
    for (int poll = 0; poll < k_polls; ++poll)
    {
        stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(k_firstPollId + poll) +
                    R"(,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                    fixture.Uri("main.as") + R"("}}})");
        stream.PushAction([]() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    }

    // Read before the shutdown: after it the analysis thread is stopped, and a transcript examined
    // then would not say whether the diagnostic arrived while the user was typing or on the way out.
    std::string framesWhilePolling;
    stream.PushAction([&]() { framesWhilePolling = PublishedFrames(stream.Output()); });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    // `justTyped` exists only in the buffer the edit created, so this code cannot have come from
    // the file on disk or from the text that was opened.
    INFO(framesWhilePolling);
    CHECK(framesWhilePolling.find("as-warn-unused-variable") != std::string::npos);

    // And the client was answered rather than told to ask again for the whole second.
    const std::string lastPoll = stream.ResponseFor(k_firstPollId + k_polls - 1);
    INFO(lastPoll);
    CHECK(lastPoll.find("\"result\"") != std::string::npos);
}

// =====================================================================================
// The command the stub picker is built on.
//
// `angelscript.listPredefinedStubs` is how the client learns which stubs exist without duplicating
// this server's rule for what counts as one - and it had no test at all, which is exactly the shape
// of thing that can be broken for a whole release without anything failing. The picker showing
// nothing and the picker not opening look identical from the outside.
// =====================================================================================

TEST_CASE("Server - Answers the stub listing the picker is built on")
{
    TwoStubFixture two;
    two.fixture.Write("main.as", "void main() { }\n");

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // One message between `initialized` and the wait, so the wait runs after initialized has been
    // handled and the scan it starts exists - see ScriptedStream::PushAction.
    stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), "void main() { }\n"));
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string answer = stream.ResponseFor(2);
    INFO(answer);

    REQUIRE(answer.find("\"error\"") == std::string::npos);
    CHECK(answer.find("host_a.as.predefined") != std::string::npos);
    CHECK(answer.find("host_b.as.predefined") != std::string::npos);

    // And it names the one actually in force, so the picker can tick it. With nothing configured
    // that is the one the scan chose, not the empty string the setting still holds.
    CHECK(answer.find("\"active\"") != std::string::npos);
    const size_t active = answer.find("\"active\"");
    REQUIRE(active != std::string::npos);
    CHECK(answer.find("host_a.as.predefined", active) != std::string::npos);
}

TEST_CASE("Server - The listing reports a merge as one")
{
    TwoStubFixture two;
    two.fixture.Write("main.as", "void main() { }\n");

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), "void main() { }\n"));
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.activePredefined = "all";
    RunScript(serverConfig, stream);

    const std::string answer = stream.ResponseFor(2);
    INFO(answer);

    // Nothing is "the active one" while everything is loaded, and the picker needs to be able to
    // tell that apart from having no answer yet.
    CHECK(answer.find("\"merging\":true") != std::string::npos);
}

// =====================================================================================
// Push and pull are alternatives, not layers.
//
// The server announces both, which is what lets it serve either kind of client. It also *sent*
// both, and a client that pulls keeps each in its own collection - so every finding appeared twice
// in the Problems panel and twice in a hover. Reported from real use, as "igual si hago hover
// aparece ese duplicado de problemas".
// =====================================================================================

TEST_CASE("Server - A client that pulls diagnostics is not also sent them")
{
    WorkspaceFixture fixture;
    const std::string source = "void main() { int unused = 1; }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
                R"("processId":null,"rootUri":")" + fixture.RootUri() + R"(",)"
                R"("capabilities":{"textDocument":{"diagnostic":{"dynamicRegistration":false}}},)"
                R"("workspaceFolders":[{"uri":")" + fixture.RootUri() + R"(","name":"fixture"}]}})");
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"textDocument/diagnostic","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("}}})");
    stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(output);

    // Nothing pushed...
    CHECK(output.find("textDocument/publishDiagnostics") == std::string::npos);

    // ...and the pull answered, so the findings did reach the client - once.
    const std::string pulled = stream.ResponseFor(2);
    INFO(pulled);
    CHECK(pulled.find("as-warn-unused-variable") != std::string::npos);
}

TEST_CASE("Server - A client that does not pull is still sent its diagnostics")
{
    // The other half, and the reason the check is on the client's capability rather than on the
    // server's own: a client that never sends textDocument/diagnostic would otherwise be told
    // nothing at all, forever.
    WorkspaceFixture fixture;
    const std::string source = "void main() { int unused = 1; }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    CHECK(Published(stream.Output(), "as-warn-unused-variable"));
}

// =====================================================================================
// Opening a stub is not the same as selecting it.
//
// The workspace scan honours the selection; the three document handlers did not, and each loaded
// whatever stub was put in front of it. So selecting host_a and then opening host_b in the editor
// put host_b's types straight back into the table, where they resolved happily - the opposite of
// what selecting one stub is for, and invisible, because the symptom is a name that *works*.
// Reported from real use: "aunque hayas seleccionado el stub igual sigue contando las entidades
// del otro as.predefined y no las marca como error".
// =====================================================================================

TEST_CASE("Server - Opening the stub that was not selected does not load it")
{
    TwoStubFixture two;
    const std::string source = "void main() { TypeFromB b; }\n";
    two.fixture.Write("main.as", source);

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // A message between initialized and the wait, so the wait runs after the scan exists - see
    // ScriptedStream::PushAction.
    stream.Push(DidOpenMessage(two.fixture.Uri("host_b.as.predefined"),
                               "class TypeFromB { void Poke(); }\n"));
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

    // Opened again after the scan has chosen, which is the order a user reaches this in: the
    // notification names host_a, they go and look at host_b.
    stream.Push(DidOpenMessage(two.fixture.Uri("host_b.as.predefined"),
                               "class TypeFromB { void Poke(); }\n"));
    stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), source));
    stream.PushAction([&]() { waitFor(stream, "publishDiagnostics"); });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    INFO(PublishedFrames(stream.Output()));

    // host_a is the one the scan picked, so TypeFromB is a type this workspace does not have.
    CHECK(Published(stream.Output(), "as-err-unresolved-type"));
}

TEST_CASE("Server - Opening the stub that was selected keeps it loaded")
{
    // The other half. A rule that kept every opened stub out of the table would be just as wrong.
    TwoStubFixture two;
    const std::string source = "void main() { TypeFromA a; }\n";
    two.fixture.Write("main.as", source);

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(two.fixture.Uri("host_a.as.predefined"),
                               "class TypeFromA { void Poke(); }\n"));
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

    stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), source));
    stream.PushAction([&]() { waitFor(stream, "publishDiagnostics"); });
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    INFO(PublishedFrames(stream.Output()));
    CHECK_FALSE(Published(stream.Output(), "as-err-unresolved-type"));
}

// =====================================================================================
// Typing, one character at a time.
//
// Every other end-to-end test in this file hands the server a finished document. An editor never
// does that: it sends one didChange per keystroke, each carrying a range rather than a whole
// document, and the server has to keep its buffer, its tree and its byte offsets in step through
// all of them. That path - ApplyIncrementalChange, ts_tree_edit, LspCharToByteColumn - had no test
// at all, and the bugs it can produce are the ones that look like the analyzer being wrong about
// code that is plainly fine.
//
// The scenarios live in tests/fixtures/typing_scenarios.json. They were generated, then replayed in
// Python and handed to the compiler: the document each one ends at is measured, so an expectation
// that disagrees with the compiler was caught before it got here.
// =====================================================================================

namespace
{
    /** \nbrief One typed character, as the editor sends it: a zero-width range and the text. */
    std::string TypeCharMessage(const std::string &uri, int version,
                                uint32_t line, uint32_t character, char typed)
    {
        const std::string position = R"({"line":)" + std::to_string(line) +
                                     R"(,"character":)" + std::to_string(character) + R"(})";

        return R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
               uri + R"(","version":)" + std::to_string(version) + R"(},"contentChanges":[{"range":{"start":)" +
               position + R"(,"end":)" + position + R"(},"text":")" + JsonEscape(std::string(1, typed)) +
               R"("}]}})";
    }

    struct TypingEdit
    {
        uint32_t line = 0;
        uint32_t character = 0;
        std::string text;
    };

    struct TypingScenario
    {
        std::string name;
        std::string why;
        std::string initial;
        std::vector<TypingEdit> edits;
        std::vector<std::string> expectPresent;
        std::vector<std::string> expectAbsent;

        bool hasHover = false;
        uint32_t hoverLine = 0;
        uint32_t hoverCharacter = 0;
        std::string hoverContains;
    };

    std::vector<TypingScenario> LoadTypingScenarios()
    {
        const std::filesystem::path path =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "typing_scenarios.json";

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("scenarios");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<TypingScenario> scenarios;
        for (const auto &entry : list->array())
        {
            REQUIRE(entry.isObject());
            const lsp::json::Object &fields = entry.object();

            TypingScenario scenario;
            scenario.name = fields.find("name")->string();
            scenario.why = fields.find("why")->string();
            scenario.initial = fields.find("initial")->string();

            for (const auto &edit : fields.find("edits")->array())
            {
                const lsp::json::Object &editFields = edit.object();
                TypingEdit typed;
                typed.line = static_cast<uint32_t>(editFields.find("line")->number());
                typed.character = static_cast<uint32_t>(editFields.find("character")->number());
                typed.text = editFields.find("text")->string();
                scenario.edits.push_back(std::move(typed));
            }

            for (const auto &code : fields.find("expectPresent")->array())
                scenario.expectPresent.push_back(code.string());
            for (const auto &code : fields.find("expectAbsent")->array())
                scenario.expectAbsent.push_back(code.string());

            if (const auto *hover = fields.find("hover"); hover && hover->isObject())
            {
                scenario.hasHover = true;
                scenario.hoverLine = static_cast<uint32_t>(hover->object().find("line")->number());
                scenario.hoverCharacter = static_cast<uint32_t>(hover->object().find("character")->number());
                scenario.hoverContains = hover->object().find("contains")->string();
            }

            scenarios.push_back(std::move(scenario));
        }

        return scenarios;
    }

    /**
     * \nbrief The body of the last publishDiagnostics frame naming this URI.
     *
     * The last, not any: typing produces one per analysis, and only the final one describes the
     * document the assertions are about.
     */
    /**
     * @brief How many publishDiagnostics frames for one file the server has written so far.
     *
     * Exists because "the stream went quiet" is not the same statement as "the server finished".
     * A step's analysis is debounced by 200ms and then has to run, so on a loaded machine the
     * output is quiet for the simple reason that the work has not started - and a harness that
     * reads at that moment gets the PREVIOUS step's diagnostics, which is a green test locally and
     * a failure on CI. That is exactly what happened: Windows CI, four ctest jobs in parallel, and
     * the assertion reported step 1's unbalanced-brace error against step 2's expectations.
     *
     * Counting publishes turns the wait into a statement about what arrived rather than about what
     * did not.
     */
    size_t CountPublishedFor(const std::string &output, const std::string &uriFragment)
    {
        size_t count = 0;
        size_t pos = 0;
        while (pos < output.size())
        {
            const size_t headerStart = output.find("Content-Length:", pos);
            if (headerStart == std::string::npos)
                break;

            const size_t bodyStart = output.find("\r\n\r\n", headerStart);
            if (bodyStart == std::string::npos)
                break;

            const size_t contentStart = bodyStart + 4;
            const size_t nextHeader = output.find("Content-Length:", contentStart);
            const size_t bodyLength =
                (nextHeader == std::string::npos) ? (output.size() - contentStart) : (nextHeader - contentStart);

            const std::string frame = output.substr(contentStart, bodyLength);
            if (frame.find("textDocument/publishDiagnostics") != std::string::npos &&
                frame.find(uriFragment) != std::string::npos)
            {
                ++count;
            }

            pos = (nextHeader == std::string::npos) ? output.size() : nextHeader;
        }
        return count;
    }

    std::string LastPublishedFor(const std::string &output, const std::string &uriFragment)
    {
        std::string last;
        size_t pos = 0;
        while (pos < output.size())
        {
            const size_t headerStart = output.find("Content-Length:", pos);
            if (headerStart == std::string::npos)
                break;

            const size_t bodyStart = output.find("\r\n\r\n", headerStart);
            if (bodyStart == std::string::npos)
                break;

            const size_t contentStart = bodyStart + 4;
            const size_t nextHeader = output.find("Content-Length:", contentStart);
            const size_t bodyLength =
                (nextHeader == std::string::npos) ? (output.size() - contentStart) : (nextHeader - contentStart);

            const std::string frame = output.substr(contentStart, bodyLength);
            if (frame.find("textDocument/publishDiagnostics") != std::string::npos &&
                frame.find(uriFragment) != std::string::npos)
            {
                last = frame;
            }

            pos = contentStart + bodyLength;
        }
        return last;
    }
}

TEST_CASE("Server - Diagnostics and hover survive being typed one character at a time")
{
    const std::vector<TypingScenario> scenarios = LoadTypingScenarios();
    REQUIRE_FALSE(scenarios.empty());

    for (const TypingScenario &scenario : scenarios)
    {
        CAPTURE(scenario.name);
        INFO(scenario.why);

        WorkspaceFixture fixture;
        fixture.Write("main.as", scenario.initial);

        test::ScriptedStream stream;
        stream.Push(InitializeMessage(fixture.RootUri()));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), scenario.initial));

        // One notification per character, each with the range the cursor was at - which is the
        // whole point: a whole-document change would never exercise the incremental path.
        int version = 2;
        for (const TypingEdit &edit : scenario.edits)
        {
            uint32_t line = edit.line;
            uint32_t character = edit.character;

            for (const char typed : edit.text)
            {
                stream.Push(TypeCharMessage(fixture.Uri("main.as"), version++, line, character, typed));

                if (typed == '\n')
                {
                    ++line;
                    character = 0;
                }
                else
                {
                    ++character;
                }
            }
        }

        // Analysis is debounced, so the last keystroke's diagnostics arrive after a quiet period.
        // Waiting for the stream to go quiet rather than for a fixed delay: the debounce is 200ms
        // and the analysis itself is not instant, and a fixed sleep would be either flaky or slow.
        stream.PushAction([&stream]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            size_t lastSize = 0;
            auto quietSince = std::chrono::steady_clock::now();

            while (std::chrono::steady_clock::now() < deadline)
            {
                const size_t size = stream.Output().size();
                if (size != lastSize)
                {
                    lastSize = size;
                    quietSince = std::chrono::steady_clock::now();
                }
                else if (std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(400))
                {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        // Asked after the wait above, not before it. Analysis is debounced, so a hover sent in
        // the same breath as the last keystroke is answered against a document the analyzer has
        // not reached yet - and comes back null. This used to work by accident: PushAction fired
        // before the message it was pushed behind was dispatched, so the wait ran first anyway.
        if (scenario.hasHover)
        {
            stream.Push(R"({"jsonrpc":"2.0","id":50,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
                        fixture.Uri("main.as") + R"("},"position":{"line":)" + std::to_string(scenario.hoverLine) +
                        R"(,"character":)" + std::to_string(scenario.hoverCharacter) + R"(}}})");
        }

        stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        const std::string published = LastPublishedFor(stream.Output(), "main.as");
        INFO("published: " << published);
        REQUIRE_FALSE(published.empty());

        for (const std::string &code : scenario.expectPresent)
        {
            INFO("expected present: " << code);
            CHECK(published.find("\"" + code + "\"") != std::string::npos);
        }

        for (const std::string &code : scenario.expectAbsent)
        {
            INFO("expected absent: " << code);
            CHECK(published.find("\"" + code + "\"") == std::string::npos);
        }

        if (scenario.hasHover)
        {
            const std::string hover = stream.ResponseFor(50);
            INFO("hover: " << hover);
            CHECK(hover.find(scenario.hoverContains) != std::string::npos);
        }
    }
}

TEST_CASE("Server - A message a typed diagnostic carries is the one the user reads")
{
    // The codes are asserted everywhere; the sentence beside them almost nowhere. A code with a
    // missing or malformed message reaches the user as a raw identifier, and every assertion on the
    // code alone still passes.
    WorkspaceFixture fixture;
    const std::string initial = "void test()\n{\n    \n}\n";
    fixture.Write("main.as", initial);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), initial));

    const std::string typed = "int counter = 1;";
    uint32_t character = 4;
    int version = 2;
    for (const char ch : typed)
    {
        stream.Push(TypeCharMessage(fixture.Uri("main.as"), version++, 2, character++, ch));
    }

    stream.Push(R"({"jsonrpc":"2.0","id":50,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":")" +
                fixture.Uri("main.as") + R"("}}})");

    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("as-warn-unused-variable"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO(published);

    // The sentence, and the name of the variable inside it. Not the code.
    CHECK(published.find("Local variable 'counter' is never used") != std::string::npos);

    // And no diagnostic reaches the client as a bare code with no message.
    CHECK(published.find("\"message\":\"as-") == std::string::npos);
}

// =====================================================================================
// Two files, one module, one namespace reopened in both.
//
// AngelScript compiles a module out of several sections, and a name declared twice across two of
// them is a redeclaration - measured, `namespace NS { void worker() {} }` in a file and in the file
// it includes is "A function with the same name and parameters already exists". Two files that
// never reach each other are two modules and may each declare it, which is why the rule reads the
// include closure rather than the workspace.
//
// The cases were generated, then measured: each was handed to the compiler as a directory, so the
// accept/reject beside each one is the compiler's answer and not a guess. The three that accept are
// the reason this is not a workspace-wide rule.
// =====================================================================================

namespace
{
    struct ModuleCase
    {
        std::string name;
        std::string why;
        std::string compiler;
        std::string main;
        std::string other;
        std::vector<std::string> expectPresent;
        std::vector<std::string> expectAbsent;
    };

    std::vector<ModuleCase> LoadModuleCases()
    {
        const std::filesystem::path path =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "module_cases.json";

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("cases");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<ModuleCase> cases;
        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            ModuleCase moduleCase;
            moduleCase.name = fields.find("name")->string();
            moduleCase.why = fields.find("why")->string();
            moduleCase.compiler = fields.find("compiler")->string();
            moduleCase.main = fields.find("main")->string();
            moduleCase.other = fields.find("other")->string();

            for (const auto &code : fields.find("expectPresent")->array())
                moduleCase.expectPresent.push_back(code.string());
            for (const auto &code : fields.find("expectAbsent")->array())
                moduleCase.expectAbsent.push_back(code.string());

            cases.push_back(std::move(moduleCase));
        }

        return cases;
    }
}

TEST_CASE("Server - A namespace reopened in two files of one module")
{
    const std::vector<ModuleCase> cases = LoadModuleCases();
    REQUIRE_FALSE(cases.empty());

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    for (const ModuleCase &moduleCase : cases)
    {
        CAPTURE(moduleCase.name);
        INFO(moduleCase.why);
        INFO("the compiler " << moduleCase.compiler << "s this");

        WorkspaceFixture fixture;
        fixture.Write("other.as", moduleCase.other);
        fixture.Write("main.as", moduleCase.main);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

        // A message between initialized and the wait, so the wait runs after the scan exists - see
        // ScriptedStream::PushAction. The include graph is what makes the module decidable, and it
        // is built by that scan, so opening the document before it finishes would measure a
        // workspace with no modules in it.
        stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                    R"("params":{"command":"angelscript.listPredefinedStubs"}})");
        stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

        stream.Push(DidOpenMessage(fixture.Uri("main.as"), moduleCase.main));
        stream.PushAction([&]() { waitFor(stream, "publishDiagnostics"); });
        stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        const std::string published = LastPublishedFor(stream.Output(), "main.as");
        INFO("published: " << published);
        REQUIRE_FALSE(published.empty());

        for (const std::string &code : moduleCase.expectPresent)
        {
            INFO("expected present: " << code);
            CHECK(published.find("\"" + code + "\"") != std::string::npos);
        }

        for (const std::string &code : moduleCase.expectAbsent)
        {
            INFO("expected absent: " << code);
            CHECK(published.find("\"" + code + "\"") == std::string::npos);
        }
    }
}

TEST_CASE("Server - A header included by many files is not a redeclaration")
{
    // The hazard the cross-file rule had to be built around, and the reason ValidateDuplicates used
    // to skip every foreign file outright: a shared header is indexed once and then reached from
    // every file in the module. Seeing its declarations from twenty different documents is what an
    // #include looks like from the inside - not twenty redeclarations.
    //
    // Twenty files rather than two, because the bug this guards against would grow with the count:
    // a rule comparing every pair would report here and nowhere else.
    WorkspaceFixture fixture;
    fixture.Write("common.as",
                  "namespace Core\n"
                  "{\n"
                  "    class Entity { int id; }\n"
                  "    int Helper(int v) { return v + 1; }\n"
                  "}\n"
                  "class GlobalThing { float x; }\n"
                  "int GlobalHelper() { return 7; }\n");

    std::vector<std::string> users;
    for (int i = 1; i <= 20; ++i)
    {
        const std::string name = "user" + std::to_string(i) + ".as";
        const std::string source =
            "#include \"common.as\"\n"
            "void Use" + std::to_string(i) + "()\n"
            "{\n"
            "    Core::Entity e;\n"
            "    e.id = Core::Helper(" + std::to_string(i) + ");\n"
            "    GlobalThing g;\n"
            "    g.x = float(GlobalHelper());\n"
            "}\n";

        fixture.Write(name, source);
        users.push_back(source);
    }

    const auto waitFor = [](test::ScriptedStream &stream, const std::string &needle)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains(needle))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // A message between initialized and the wait - see ScriptedStream::PushAction. The include
    // graph is what makes a module decidable and the scan is what builds it.
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { waitFor(stream, "\"kind\":\"end\""); });

    for (size_t i = 0; i < users.size(); ++i)
    {
        stream.Push(DidOpenMessage(fixture.Uri("user" + std::to_string(i + 1) + ".as"), users[i]));
    }

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(PublishedFrames(output));

    CHECK_FALSE(Published(output, "as-err-duplicate-symbol"));
    CHECK_FALSE(Published(output, "as-err-name-conflict"));

    // And the header really was reached, or the check above would pass for the wrong reason.
    CHECK_FALSE(Published(output, "as-warn-include-not-found"));
}

// =====================================================================================
// The stub in force is deleted from disk.
//
// Unloading it was already handled: its symbols go, and every open document is re-diagnosed. What
// was missing is the other half - with nothing configured, *which* stub is loaded is a choice the
// workspace scan made, and deleting that file unmakes it. Nothing re-made it, so a workspace with
// two stubs, one of them deleted, ended up with no host types at all while the other one was still
// sitting there.
// =====================================================================================

TEST_CASE("Server - Deleting the stub in force hands the workspace to the next one")
{
    TwoStubFixture two;
    const std::string source = "void main() { TypeFromB b; }\n";
    two.fixture.Write("main.as", source);

    const auto waitForCount = [](test::ScriptedStream &stream, const std::string &needle, size_t times)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::string output = stream.Output();
            size_t count = 0;
            for (size_t at = output.find(needle); at != std::string::npos;
                 at = output.find(needle, at + needle.size()))
            {
                ++count;
            }
            if (count >= times)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

    // A message between initialized and the wait - see ScriptedStream::PushAction.
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { waitForCount(stream, "\"kind\":\"end\"", 1); });

    // host_a sorts first, so it is the one the scan chose and TypeFromB does not exist yet.
    stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), source));
    stream.PushAction([&]() { waitForCount(stream, "publishDiagnostics", 1); });

    // And now the chosen one is gone.
    stream.PushAction([dir = two.fixture.dir]()
    {
        std::filesystem::remove(dir / "host_a.as.predefined");
    });

    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
                R"({"uri":")" + two.fixture.Uri("host_a.as.predefined") + R"(","type":3}]}})");

    // A message between the notification and the wait, so the wait runs after the deletion has been
    // *handled* rather than merely read - see ScriptedStream::PushAction.
    stream.Push(R"({"jsonrpc":"2.0","id":1001,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");

    // The replacement is chosen on the message loop and the open documents are refreshed against
    // it, so a second publish is how the server says it has caught up.
    stream.PushAction([&]() { waitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO("last published: " << published);
    INFO("everything: " << PublishedFrames(stream.Output()));
    REQUIRE_FALSE(published.empty());

    // host_b took over, so the type it declares resolves now. Before this, deleting host_a left the
    // workspace with nothing at all.
    CHECK(published.find("as-err-unresolved-type") == std::string::npos);
}

// =====================================================================================
// The lines the preprocessor drops, told to the client so it can dim them.
//
// This used to be done with semantic tokens - one full-line comment token per dead line - and it
// could not work: the editor paints `(`, `{` and `[` from its own bracket-pair colouring, which
// consults neither TextMate nor semantic scopes, so dead code kept rainbow brackets whatever was
// emitted for it. A decoration sits over all of it. The notification is custom because LSP has
// none, and it is what the C++ extension does with its own inactive regions.
// =====================================================================================

TEST_CASE("Server - Reports the lines a dropped #if takes with it")
{
    WorkspaceFixture fixture;
    const std::string source =
        "int live = 1;\n"
        "#if NOT_DEFINED\n"
        "int dead = 2;\n"
        "#endif\n"
        "int alsoLive = 3;\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(output);

    REQUIRE(output.find("angelscript/inactiveRegions") != std::string::npos);

    // Lines 1 to 3: the `#if`, the code it drops, and the `#endif` - CScriptBuilder blanks the
    // directives too, so they are part of the region.
    CHECK(output.find("\"startLine\":1") != std::string::npos);
    CHECK(output.find("\"endLine\":3") != std::string::npos);
}

TEST_CASE("Server - Reports an empty region list when nothing is dropped")
{
    // Sent even when empty: that is how the client learns a block came back to life and its dimming
    // has to go. Without it the grey would stay until the document was closed.
    WorkspaceFixture fixture;
    const std::string source = "int live = 1;\nint alsoLive = 2;\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeMessage(fixture.RootUri()));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(output);

    REQUIRE(output.find("angelscript/inactiveRegions") != std::string::npos);
    CHECK(output.find("\"regions\":[]") != std::string::npos);
}

// =====================================================================================
// What the server says while the code is still broken.
//
// The typing scenarios above check the document once, when the typing stops. A user does not live
// there: most of the time what is on screen is half a statement, an unclosed brace, a type that
// does not exist yet. This checks the server after EVERY step, so what it says mid-sentence is
// pinned rather than assumed - including the message text, which is the part the user actually
// reads.
//
// A scenario may carry a `predefined` stub, written into the workspace before the server starts.
// That is the real shape of the thing: host types exist, and a name that would be undeclared in a
// bare workspace resolves in a real one.
// =====================================================================================

namespace
{
    struct BrokenStep
    {
        std::string typed;
        uint32_t line = 0;
        uint32_t character = 0;
        std::vector<std::string> expectPresent;
        std::vector<std::string> expectAbsent;
        std::string message;
    };

    struct BrokenScenario
    {
        std::string name;
        std::string why;
        std::string initial;
        std::string predefined;   ///< Empty when the scenario needs no host stub.
        std::vector<BrokenStep> steps;
    };

    std::vector<BrokenScenario> LoadBrokenScenarios()
    {
        const std::filesystem::path path =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "typing_broken_scenarios.json";

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("scenarios");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<BrokenScenario> scenarios;
        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            BrokenScenario scenario;
            scenario.name = fields.find("name")->string();
            scenario.why = fields.find("why")->string();
            scenario.initial = fields.find("initial")->string();

            if (const auto *stub = fields.find("predefined"); stub && stub->isString())
                scenario.predefined = stub->string();

            for (const auto &item : fields.find("steps")->array())
            {
                const lsp::json::Object &stepFields = item.object();

                BrokenStep step;
                step.typed = stepFields.find("type")->string();
                step.line = static_cast<uint32_t>(stepFields.find("line")->number());
                step.character = static_cast<uint32_t>(stepFields.find("character")->number());

                for (const auto &code : stepFields.find("expectPresent")->array())
                    step.expectPresent.push_back(code.string());
                for (const auto &code : stepFields.find("expectAbsent")->array())
                    step.expectAbsent.push_back(code.string());

                if (const auto *message = stepFields.find("message"); message && message->isString())
                    step.message = message->string();

                scenario.steps.push_back(std::move(step));
            }

            scenarios.push_back(std::move(scenario));
        }

        return scenarios;
    }
}

TEST_CASE("Server - What it says while the code is still being written")
{
    const std::vector<BrokenScenario> scenarios = LoadBrokenScenarios();
    REQUIRE_FALSE(scenarios.empty());

    size_t stepsChecked = 0;

    for (const BrokenScenario &scenario : scenarios)
    {
        CAPTURE(scenario.name);
        INFO(scenario.why);

        WorkspaceFixture fixture;
        if (!scenario.predefined.empty())
            fixture.Write("host.as.predefined", scenario.predefined);
        fixture.Write("main.as", scenario.initial);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

        // A message between initialized and the wait, so the wait runs after the scan that loads
        // the stub has started - see ScriptedStream::PushAction.
        stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                    R"("params":{"command":"angelscript.listPredefinedStubs"}})");
        stream.PushAction([&]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains("\"kind\":\"end\""))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        stream.Push(DidOpenMessage(fixture.Uri("main.as"), scenario.initial));

        // One slot per step, filled by the action that runs once that step's keystrokes have been
        // handled. Reserved up front: the lambdas capture a pointer into it.
        std::vector<std::string> published(scenario.steps.size());

        // Publishes for main.as seen before the current step's keystrokes. Carried across steps so
        // each wait can tell a publish that answers THIS step from one left over from the last.
        size_t publishesBefore = 0;

        // The didOpen above produces a publish of its own, for the untouched document. Starting the
        // count at zero made that publish look like step 0's answer, and on a slow machine it lands
        // during step 0's wait - so the harness read an empty diagnostic list and checked it against
        // expectations for text that had not been analysed yet.
        //
        // The filler request matters as much as the wait. A PushAction runs on the reader's thread,
        // at the moment the bytes before it are consumed and BEFORE the server has dispatched them,
        // and didOpen is handled on that same message loop. An action that blocks waiting for
        // didOpen's publish therefore blocks the loop that would produce it - it cannot succeed, and
        // it burned the full timeout in every scenario, taking this test from 28 seconds to five
        // minutes. Putting a request between the two is what lets didOpen be handled first.
        stream.Push(R"({"jsonrpc":"2.0","id":1500,"method":"textDocument/documentSymbol",)"
                    R"("params":{"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("}}})");

        stream.PushAction([&stream, &publishesBefore]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (CountPublishedFor(stream.Output(), "main.as") > 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            publishesBefore = CountPublishedFor(stream.Output(), "main.as");
        });

        int version = 2;
        int requestId = 2000;

        for (size_t index = 0; index < scenario.steps.size(); ++index)
        {
            const BrokenStep &step = scenario.steps[index];

            uint32_t line = step.line;
            uint32_t character = step.character;

            for (const char typed : step.typed)
            {
                stream.Push(TypeCharMessage(fixture.Uri("main.as"), version++, line, character, typed));

                if (typed == '\n')
                {
                    ++line;
                    character = 0;
                }
                else
                {
                    ++character;
                }
            }

            // The filler that makes the wait below run after the last keystroke was handled, not
            // merely read.
            stream.Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(requestId++) +
                        R"(,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":")" +
                        fixture.Uri("main.as") + R"("}}})");

            stream.PushAction([&stream, &published, &publishesBefore, index, &fixture]()
            {
                // Two conditions, and the first one is the fix. Waiting only for the stream to go
                // quiet reads "nothing has been written lately", which is true both when the server
                // has finished and when its debounced analysis has not started - and on a loaded
                // machine the second is what happens. Windows CI, four ctest jobs in parallel: this
                // read step 1's diagnostics and checked them against step 2's expectations.
                //
                // So wait for a publish that did not exist before this keystroke, and only then for
                // the stream to settle, which catches a later republish. The deadline is the
                // backstop; reaching it means something is genuinely wrong rather than slow.
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                size_t lastSize = 0;
                auto quietSince = std::chrono::steady_clock::now();
                bool sawNewPublish = false;

                while (std::chrono::steady_clock::now() < deadline)
                {
                    const std::string output = stream.Output();

                    if (!sawNewPublish && CountPublishedFor(output, "main.as") > publishesBefore)
                    {
                        sawNewPublish = true;
                        quietSince = std::chrono::steady_clock::now();
                    }

                    const size_t size = output.size();
                    if (size != lastSize)
                    {
                        lastSize = size;
                        quietSince = std::chrono::steady_clock::now();
                    }
                    else if (sawNewPublish &&
                             std::chrono::steady_clock::now() - quietSince > std::chrono::milliseconds(400))
                    {
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }

                published[index] = LastPublishedFor(stream.Output(), "main.as");
                publishesBefore = CountPublishedFor(stream.Output(), "main.as");
            });
        }

        stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        for (size_t index = 0; index < scenario.steps.size(); ++index)
        {
            const BrokenStep &step = scenario.steps[index];

            CAPTURE(index);
            CAPTURE(step.typed);
            INFO("published: " << published[index]);

            REQUIRE_FALSE(published[index].empty());

            for (const std::string &code : step.expectPresent)
            {
                INFO("expected present: " << code);
                CHECK(published[index].find("\"" + code + "\"") != std::string::npos);
            }

            for (const std::string &code : step.expectAbsent)
            {
                INFO("expected absent: " << code);
                CHECK(published[index].find("\"" + code + "\"") == std::string::npos);
            }

            if (!step.message.empty())
            {
                INFO("expected message to contain: " << step.message);
                CHECK(published[index].find(step.message) != std::string::npos);
            }

            ++stepsChecked;
        }
    }

    MESSAGE("half-written code: " << stepsChecked << " steps checked");
}

// =====================================================================================
// Where the squiggle sits.
//
// Every other test here asks whether a problem is reported. None asks where. A diagnostic that
// points at the whole line, or at the token after the mistake, is useless even when its message is
// right - the reader has to find the defect themselves, which is the work the tool was for.
//
// The expectations name the TEXT the range must span rather than four numbers, so they read as what
// a reader should see underlined and cannot drift into asserting an off-by-one nobody notices.
// =====================================================================================

namespace
{
    struct RangeExpectation
    {
        std::string code;
        std::string covers;
        uint32_t line = 0;
    };

    struct RangeScenario
    {
        std::string name;
        std::string why;
        std::string source;
        std::vector<RangeExpectation> expect;
    };

    std::vector<RangeScenario> LoadRangeScenarios()
    {
        const std::filesystem::path path =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "range_scenarios.json";

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("scenarios");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<RangeScenario> scenarios;
        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            RangeScenario scenario;
            scenario.name = fields.find("name")->string();
            scenario.why = fields.find("why")->string();
            scenario.source = fields.find("source")->string();

            for (const auto &item : fields.find("expect")->array())
            {
                const lsp::json::Object &expectFields = item.object();

                RangeExpectation expectation;
                expectation.code = expectFields.find("code")->string();
                expectation.covers = expectFields.find("covers")->string();
                expectation.line = static_cast<uint32_t>(expectFields.find("line")->number());
                scenario.expect.push_back(std::move(expectation));
            }

            scenarios.push_back(std::move(scenario));
        }

        return scenarios;
    }

    struct PublishedDiagnostic
    {
        std::string code;
        uint32_t startLine = 0;
        uint32_t startCharacter = 0;
        uint32_t endLine = 0;
        uint32_t endCharacter = 0;
    };

    /** @brief The diagnostics inside one publishDiagnostics frame, parsed rather than grepped. */
    std::vector<PublishedDiagnostic> ParsePublished(const std::string &frame)
    {
        std::vector<PublishedDiagnostic> parsed;
        if (frame.empty())
            return parsed;

        lsp::json::Value message = lsp::json::parse(frame);
        if (!message.isObject())
            return parsed;

        const auto *params = message.object().find("params");
        if (params == nullptr || !params->isObject())
            return parsed;

        const auto *list = params->object().find("diagnostics");
        if (list == nullptr || !list->isArray())
            return parsed;

        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            PublishedDiagnostic diagnostic;
            if (const auto *code = fields.find("code"); code && code->isString())
                diagnostic.code = code->string();

            const auto *range = fields.find("range");
            if (range == nullptr || !range->isObject())
                continue;

            const auto *start = range->object().find("start");
            const auto *end = range->object().find("end");
            if (start == nullptr || end == nullptr)
                continue;

            diagnostic.startLine = static_cast<uint32_t>(start->object().find("line")->number());
            diagnostic.startCharacter = static_cast<uint32_t>(start->object().find("character")->number());
            diagnostic.endLine = static_cast<uint32_t>(end->object().find("line")->number());
            diagnostic.endCharacter = static_cast<uint32_t>(end->object().find("character")->number());

            parsed.push_back(std::move(diagnostic));
        }

        return parsed;
    }

    /** @brief The source text a range spans, or empty when the range does not fit the document. */
    std::string TextInRange(const std::string &source, const PublishedDiagnostic &diagnostic)
    {
        std::vector<size_t> lineStarts{ 0 };
        for (size_t at = 0; at < source.size(); ++at)
        {
            if (source[at] == '\n')
                lineStarts.push_back(at + 1);
        }

        if (diagnostic.startLine >= lineStarts.size() || diagnostic.endLine >= lineStarts.size())
            return {};

        const size_t from = lineStarts[diagnostic.startLine] + diagnostic.startCharacter;
        const size_t to = lineStarts[diagnostic.endLine] + diagnostic.endCharacter;

        if (from > to || to > source.size())
            return {};

        return source.substr(from, to - from);
    }
}

TEST_CASE("Server - A diagnostic underlines the text it is about")
{
    const std::vector<RangeScenario> scenarios = LoadRangeScenarios();
    REQUIRE_FALSE(scenarios.empty());

    size_t checked = 0;

    for (const RangeScenario &scenario : scenarios)
    {
        CAPTURE(scenario.name);
        INFO(scenario.why);

        WorkspaceFixture fixture;
        fixture.Write("main.as", scenario.source);

        test::ScriptedStream stream;
        stream.Push(InitializeMessage(fixture.RootUri()));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), scenario.source));
        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        const std::string frame = LastPublishedFor(stream.Output(), "main.as");
        INFO("published: " << frame);
        REQUIRE_FALSE(frame.empty());

        const auto diagnostics = ParsePublished(frame);

        for (const RangeExpectation &expectation : scenario.expect)
        {
            CAPTURE(expectation.code);
            CAPTURE(expectation.line);
            INFO("should underline: '" << expectation.covers << "'");

            // Any diagnostic of that code on that line may be the one meant: two unused locals
            // declared together sit on one line, and which of them comes first in the payload is
            // not something an expectation should have to know. The underline is what identifies
            // it, so the match is on the underline.
            std::vector<std::string> underlinedHere;
            bool matched = false;

            for (const PublishedDiagnostic &diagnostic : diagnostics)
            {
                if (diagnostic.code != expectation.code || diagnostic.startLine != expectation.line)
                    continue;

                const std::string underlined = TextInRange(scenario.source, diagnostic);
                underlinedHere.push_back(underlined);

                if (underlined == expectation.covers)
                {
                    matched = true;
                    break;
                }
            }

            if (underlinedHere.empty())
            {
                FAIL_CHECK("no " << expectation.code << " reported on line " << expectation.line);
                continue;
            }

            std::string actually;
            for (const std::string &text : underlinedHere)
            {
                if (!actually.empty())
                    actually += "', '";
                actually += text;
            }

            INFO("actually underlined: '" << actually << "'");
            CHECK(matched);

            ++checked;
        }
    }

    MESSAGE("diagnostic ranges: " << checked << " underlines checked");
}

// =====================================================================================
// Files appearing and disappearing under the server.
//
// A workspace is not a fixed set of files: a branch switch, a pull, a `git checkout` or a plain
// delete all happen while the server is running, and it hears about them as
// workspace/didChangeWatchedFiles. Deleting the stub in force is covered above; these are the other
// four corners - a script created, a script deleted, a stub created where there was none, and the
// only stub deleted.
// =====================================================================================

namespace
{
    /** \nbrief One watched-file event, in the shape the client sends. */
    std::string WatchedFileMessage(const std::string &uri, int changeType)
    {
        return R"({"jsonrpc":"2.0","method":"workspace/didChangeWatchedFiles","params":{"changes":[)"
               R"({"uri":")" + uri + R"(","type":)" + std::to_string(changeType) + R"(}]}})";
    }

    /** \nbrief Waits until a needle has appeared at least `times` times. */
    void WaitForCount(test::ScriptedStream &stream, const std::string &needle, size_t times)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const std::string output = stream.Output();
            size_t count = 0;
            for (size_t at = output.find(needle); at != std::string::npos;
                 at = output.find(needle, at + needle.size()))
            {
                ++count;
            }
            if (count >= times)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

TEST_CASE("Server - A script created on disk becomes part of the module")
{
    // main.as includes helper.as, which does not exist yet. Once it appears the type it declares has
    // to resolve, without the user touching main.as.
    WorkspaceFixture fixture;
    const std::string source =
        "#include \"helper.as\"\n"
        "void main() { HelperType h; h.Poke(); }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 1); });

    // The file arrives.
    stream.PushAction([dir = fixture.dir]()
    {
        std::ofstream created(dir / "helper.as", std::ios::binary);
        created << "class HelperType { void Poke() { } }\n";
    });

    stream.Push(WatchedFileMessage(fixture.Uri("helper.as"), /*Created=*/1));
    stream.Push(R"({"jsonrpc":"2.0","id":1001,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO("last published: " << published);
    INFO("everything: " << PublishedFrames(stream.Output()));
    REQUIRE_FALSE(published.empty());

    CHECK(published.find("as-err-unresolved-type") == std::string::npos);
    CHECK(published.find("as-warn-include-not-found") == std::string::npos);
}

TEST_CASE("Server - A script deleted on disk stops resolving for the file that included it")
{
    // The other direction. helper.as exists at the start and goes away.
    WorkspaceFixture fixture;
    const std::string source =
        "#include \"helper.as\"\n"
        "void main() { HelperType h; h.Poke(); }\n";
    fixture.Write("helper.as", "class HelperType { void Poke() { } }\n");
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 1); });

    stream.PushAction([dir = fixture.dir]() { std::filesystem::remove(dir / "helper.as"); });

    stream.Push(WatchedFileMessage(fixture.Uri("helper.as"), /*Deleted=*/3));
    stream.Push(R"({"jsonrpc":"2.0","id":1001,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO("last published: " << published);
    REQUIRE_FALSE(published.empty());

    // The type it declared is gone, and the include no longer resolves. Either is enough to say the
    // deletion was noticed; both are what the user would see.
    const bool typeGone = published.find("as-err-unresolved-type") != std::string::npos;
    const bool includeGone = published.find("as-warn-include-not-found") != std::string::npos;
    CHECK((typeGone || includeGone));
}

// =====================================================================================
// A document opened before the table was built.
//
// Measured on a real Sven Co-op project: 258 diagnostics, every one of them a host type reported
// unknown, on code the compiler builds. Nothing was wrong with the stub - the file had simply been
// analysed before the scan finished loading it, and nothing ever looked again. Forcing one late
// analysis took the same file to 126.
//
// The scan cannot be made to finish after the didOpen on demand, so what is guarded here is the
// mechanism rather than the race: a scan that ends re-analyses whatever is open, even though not
// one byte of it changed. Remove the guard in ReadWorkspaceFiles and this stops at one publication.
// =====================================================================================
TEST_CASE("Server - A finished scan re-analyses the open documents it did not know about")
{
    WorkspaceFixture fixture;
    const std::string source = "void main() { ScanRaceHost h; h.Spawn(); }\n";
    fixture.Write("main.as", source);
    fixture.Write("host.as.predefined", "class ScanRaceHost { void Spawn(); }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 1); });

    // The same text, and a table that has moved. A dedupe that only asks "are these the same
    // bytes?" answers "already done" here, which is why ScheduleAnalysis has to be told otherwise.
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.rescanWorkspace"}})");
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    size_t publications = 0;
    for (size_t at = output.find("publishDiagnostics"); at != std::string::npos;
         at = output.find("publishDiagnostics", at + 1))
    {
        ++publications;
    }

    INFO("everything: " << PublishedFrames(output));
    CHECK(publications >= 2);

    // And the answer is still the right one: the host type resolves either way.
    const std::string published = LastPublishedFor(output, "main.as");
    REQUIRE_FALSE(published.empty());
    CHECK(published.find("as-err-unresolved-type") == std::string::npos);
}

TEST_CASE("Server - A stub created where there was none is picked up")
{
    // A workspace with no as.predefined at all: the host types do not exist, and then one does.
    WorkspaceFixture fixture;
    const std::string source = "void main() { CBaseEntity e; }\n";
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 1); });

    stream.PushAction([dir = fixture.dir]()
    {
        std::ofstream created(dir / "host.as.predefined", std::ios::binary);
        created << "class CBaseEntity { void Spawn(); }\n";
    });

    stream.Push(WatchedFileMessage(fixture.Uri("host.as.predefined"), /*Created=*/1));
    stream.Push(R"({"jsonrpc":"2.0","id":1001,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO("last published: " << published);
    INFO("everything: " << PublishedFrames(stream.Output()));
    REQUIRE_FALSE(published.empty());

    CHECK(published.find("as-err-unresolved-type") == std::string::npos);
}

TEST_CASE("Server - Deleting the only stub leaves the host types unknown")
{
    // The counterpart of the test above it, and of the one where a second stub takes over: with no
    // replacement to find, the types really do go, and saying so is right rather than a defect.
    WorkspaceFixture fixture;
    const std::string source = "void main() { CBaseEntity e; }\n";
    fixture.Write("host.as.predefined", "class CBaseEntity { void Spawn(); }\n");
    fixture.Write("main.as", source);

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.Push(R"({"jsonrpc":"2.0","id":1000,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 1); });

    stream.PushAction([dir = fixture.dir]() { std::filesystem::remove(dir / "host.as.predefined"); });

    stream.Push(WatchedFileMessage(fixture.Uri("host.as.predefined"), /*Deleted=*/3));
    stream.Push(R"({"jsonrpc":"2.0","id":1001,"method":"workspace/executeCommand",)"
                R"("params":{"command":"angelscript.listPredefinedStubs"}})");
    stream.PushAction([&]() { WaitForCount(stream, "publishDiagnostics", 2); });

    stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    RunScript(serverConfig, stream);

    const std::string published = LastPublishedFor(stream.Output(), "main.as");
    INFO("last published: " << published);
    REQUIRE_FALSE(published.empty());

    CHECK(published.find("as-err-unresolved-type") != std::string::npos);
}

// =====================================================================================
// Hover over the shapes that nest.
//
// `array<array<int>>`, a dictionary holding a dictionary, an element pulled out of an
// `array<dictionary>`, a counter three loops deep. These are where a type answer degrades quietly:
// the tooltip still appears, and says `array` or nothing useful, and nobody notices until they are
// reading someone else's code.
//
// The scripts were also handed to the compiler, and three of the generated predictions about it
// were wrong - see the `why` of each. Two of those are a property of how this engine build
// registers the array template rather than a rule of the language, which is why the compiler
// verdict is recorded in the fixture and not turned into a parity case.
// =====================================================================================

namespace
{
    struct NestedScenario
    {
        std::string name;
        std::string why;
        std::string source;

        uint32_t hoverLine = 0;
        uint32_t hoverCharacter = 0;
        std::string hoverText;
        std::string hoverContains;
    };

    std::vector<NestedScenario> LoadNestedScenarios()
    {
        const std::filesystem::path path =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "nested_scenarios.json";

        std::ifstream file(path, std::ios::binary);
        REQUIRE_MESSAGE(file.is_open(), "cannot open " << path.string());

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        lsp::json::Value parsed = lsp::json::parse(text);
        REQUIRE(parsed.isObject());

        const auto *list = parsed.object().find("scenarios");
        REQUIRE(list != nullptr);
        REQUIRE(list->isArray());

        std::vector<NestedScenario> scenarios;
        for (const auto &entry : list->array())
        {
            const lsp::json::Object &fields = entry.object();

            NestedScenario scenario;
            scenario.name = fields.find("name")->string();
            scenario.why = fields.find("why")->string();
            scenario.source = fields.find("source")->string();

            const auto *hover = fields.find("hover");
            if (hover == nullptr || !hover->isObject())
                continue;

            scenario.hoverLine = static_cast<uint32_t>(hover->object().find("line")->number());
            scenario.hoverCharacter = static_cast<uint32_t>(hover->object().find("character")->number());
            scenario.hoverText = hover->object().find("text")->string();
            scenario.hoverContains = hover->object().find("contains")->string();

            scenarios.push_back(std::move(scenario));
        }

        return scenarios;
    }
}

TEST_CASE("Server - Hover answers for the shapes that nest")
{
    const std::vector<NestedScenario> scenarios = LoadNestedScenarios();
    REQUIRE_FALSE(scenarios.empty());

    size_t answered = 0;
    size_t silent = 0;
    size_t wrong = 0;

    for (const NestedScenario &scenario : scenarios)
    {
        CAPTURE(scenario.name);
        INFO(scenario.why);

        WorkspaceFixture fixture;
        fixture.Write("main.as", scenario.source);

        test::ScriptedStream stream;
        stream.Push(InitializeMessage(fixture.RootUri()));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), scenario.source));
        stream.Push(R"({"jsonrpc":"2.0","id":50,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
                    fixture.Uri("main.as") + R"("},"position":{"line":)" + std::to_string(scenario.hoverLine) +
                    R"(,"character":)" + std::to_string(scenario.hoverCharacter) + R"(}}})");
        stream.Push(R"({"jsonrpc":"2.0","id":99,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);

        const std::string hover = stream.ResponseFor(50);
        INFO("hover: " << hover);
        INFO("should mention: " << scenario.hoverContains);

        if (hover.find("\"result\":null") != std::string::npos || hover.empty())
        {
            ++silent;
            continue;
        }

        if (hover.find(scenario.hoverContains) != std::string::npos)
            ++answered;
        else
            ++wrong;
    }

    MESSAGE("nested hover: " << answered << " of " << scenarios.size()
                             << " named the type, " << wrong << " named something else, "
                             << silent << " said nothing");

    // Every one of them, because every one of them answered when this was written: 20 of 20 named
    // the type, none said anything else, none stayed silent. A soft assertion would have been the
    // honest thing only if something here were still broken, and nothing is - so this guards it.
    CHECK(answered == scenarios.size());
    CHECK(wrong == 0);
    CHECK(silent == 0);
}

// =====================================================================================
// Switching the active stub, and the half of that fix nobody had written.
//
// "Switching engine profile forgets the profile that was left" above pins the built-in half:
// LoadBuiltinEngineProfiles releases the profile the user moved away from. The stubs on disk go
// through the same ClaimPredefinedFile path and had no release at all, so a rescan could only ever
// add - the stub that stopped being active kept every class it declared in the symbol table, kept
// answering hover, and kept its `#define`s defined.
//
// Reported from use: after switching stubs, "me sigue pudiendo hacer hover y no marca la entidad
// como si fuera invalida". Both halves of that sentence are a case below, because they fail
// separately: diagnostics and hover read the symbol table through different paths, and a fix that
// only silenced one would look right in whichever one you happened to check.
// =====================================================================================

namespace
{
    /**
     * @brief Starts under host_a, switches the selection to host_b, then opens @p source.
     *
     * The switch travels the way the client makes it - didChangeConfiguration, not a restart -
     * because that is the path with the missing unload. Restarting the server would rebuild the
     * table from nothing and pass whatever the unload path did.
     */
    std::string RunAfterSwitchingStub(const TwoStubFixture &two, const std::string &source)
    {
        two.fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");

        // The first scan loads host_a, named in the config at the bottom.
        stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

        stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":)"
                    R"({"settings":{"angelscript":{"predefined":{"active":")" +
                    JsonEscape(two.Stub("host_b.as.predefined")) + R"("}}}}})");

        // The rescan runs its own progress cycle, so wait for a second one to finish.
        stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 2); });

        stream.Push(DidOpenMessage(two.fixture.Uri("main.as"), source));
        stream.PushAction([&stream]() { WaitForCount(stream, "publishDiagnostics", 1); });

        stream.Push(R"({"jsonrpc":"2.0","id":3,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
                    two.fixture.Uri("main.as") + R"("},"position":{"line":0,"character":16}}})");

        stream.Push(R"({"jsonrpc":"2.0","id":4,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        serverConfig.activePredefined = two.Stub("host_a.as.predefined");

        // Not the default "none": the analyzer stays silent about a type whose world it cannot see,
        // and with an empty table every assertion below would pass by vacuity.
        serverConfig.engineProfile = "standard";

        RunScript(serverConfig, stream);
        return stream.Output();
    }
}

TEST_CASE("Server - Switching the active stub forgets the stub that was left")
{
    TwoStubFixture two;

    const std::string output = RunAfterSwitchingStub(two, "void main() { TypeFromA a; }\n");

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    // host_a is no longer the selection, so nothing it declares exists any more.
    CHECK(Published(output, "as-err-unresolved-type"));
}

TEST_CASE("Server - The stub that was switched to still resolves")
{
    // The other half of the pair. Without it, a server that had simply stopped loading stubs
    // altogether would pass the case above.
    TwoStubFixture two;

    const std::string output = RunAfterSwitchingStub(two, "void main() { TypeFromB b; }\n");

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-unresolved-type"));
}

TEST_CASE("Server - Hover stops describing a type the stub no longer declares")
{
    // The reported symptom, and the reason the diagnostic case above is not enough on its own: the
    // error appeared and the hover card still described the class, side by side on the same word.
    // A user reading that sees the server contradicting itself and believes the error.
    TwoStubFixture two;

    const std::string output = RunAfterSwitchingStub(two, "void main() { TypeFromA a; }\n");

    INFO(output);

    // The reply arrived at all - otherwise "no TypeFromA in the output" would be true of a server
    // that answered nothing, which is exactly how this assertion could pass while broken.
    REQUIRE(output.find(R"("id":3)") != std::string::npos);

    // The card, not the word: the published diagnostic legitimately names TypeFromA in its own
    // message, so searching the whole stream for the bare identifier would never pass.
    CHECK(output.find("class TypeFromA") == std::string::npos);
}

// =====================================================================================
// A stub edited in the editor, rather than on disk.
//
// didChangeWatchedFiles skips any document the editor has open, on purpose: the buffer wins over
// the copy on disk. But the editor path never recorded a stub's `#define`s at all - only
// ParserPredefined did, and that is the disk path. So editing a stub in a tab updated the types it
// declares and left every `#if` in every other document on the previous answer until the stub was
// reloaded by hand from the status menu.
//
// Reported from use: "si en mi stub tengo #define SERVER_BUILD y lo comento, no se actualiza al
// momento, tendria que entrar a la opcion de stub y actualizarlo".
// =====================================================================================

namespace
{
    /**
     * @brief Opens a stub and a document that depends on its `#define`, then edits the stub.
     *
     * @param editedStub What the stub becomes. The document is never touched, so any change in what
     *        is published about it came from the stub.
     * @return Everything the server wrote.
     */
    std::string RunEditingOpenStub(const std::string &editedStub, bool expectNewFrame)
    {
        const std::string stub = "#define SERVER_BUILD\nclass HostEntityA { void Spawn(); }\n";

        const std::string source =
            "#if SERVER_BUILD\n"
            "void OnServerStart()\n"
            "{\n"
            "    UndefinedThingy();\n"
            "}\n"
            "#endif\n";

        WorkspaceFixture fixture;
        fixture.Write("engine.as.predefined", stub);
        fixture.Write("main.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

        // With SERVER_BUILD defined the block is live code, so this publishes the error inside it.
        stream.Push(DidOpenMessage(fixture.Uri("main.as"), source));
        stream.PushAction([&stream]() { WaitForCount(stream, "publishDiagnostics", 1); });

        // The user opens the stub in a tab. From here on didChangeWatchedFiles will not touch it.
        stream.Push(DidOpenMessage(fixture.Uri("engine.as.predefined"), stub));
        stream.PushAction([&stream]() { WaitForCount(stream, "publishDiagnostics", 2); });

        size_t before = 0;
        stream.PushAction([&stream, &before]() { before = CountPublishedFor(stream.Output(), "main.as"); });

        stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
                    fixture.Uri("engine.as.predefined") + R"(","version":2},)"
                    R"("contentChanges":[{"text":")" + JsonEscape(editedStub) + R"("}]}})");

        // A filler request between the edit and the wait, and it is load-bearing. PushAction runs
        // on the reader thread, and it fires when the bytes before it are *consumed*, not when they
        // are handled - so an action that waits here blocks the very message loop that has to
        // dispatch the didChange above. The wait then always ran out. Reading a request back proves
        // the loop got past the edit.
        stream.Push(R"({"jsonrpc":"2.0","id":1500,"method":"textDocument/documentSymbol",)"
                    R"("params":{"textDocument":{"uri":")" + fixture.Uri("main.as") + R"("}}})");

        // Waiting for a *new* frame about main.as, not for silence: the document was already
        // published once, so "there is a publish for main.as" was true before the edit.
        stream.PushAction([&stream, &before, expectNewFrame]()
        {
            // Bounded short when no new frame is expected: waiting the full timeout for something
            // that correctly never arrives would cost fifteen seconds of suite time per run.
            const auto budget = expectNewFrame ? std::chrono::seconds(15) : std::chrono::seconds(2);
            const auto deadline = std::chrono::steady_clock::now() + budget;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (CountPublishedFor(stream.Output(), "main.as") > before)
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        RunScript(serverConfig, stream);
        return stream.Output();
    }
}

TEST_CASE("Server - Commenting out a #define in an open stub re-evaluates every #if at once")
{
    const std::string output = RunEditingOpenStub("// #define SERVER_BUILD\nclass HostEntityA { void Spawn(); }\n",
                                                    /*expectNewFrame=*/true);

    INFO(PublishedFrames(output));

    // A second frame about main.as arrived at all. Without this the assertion below would pass on a
    // server that never said anything again, which is precisely the bug being fixed.
    REQUIRE(CountPublishedFor(output, "main.as") >= 2);

    // SERVER_BUILD is gone, so the block is not code any more and nothing inside it is reported.
    const std::string last = LastPublishedFor(output, "main.as");
    INFO(last);
    CHECK(last.find(R"("diagnostics":[])") != std::string::npos);
}

TEST_CASE("Server - An edit that leaves the #defines alone does not change what the #if says")
{
    // The other direction, and the one that stops the fix from being "publish an empty list after
    // any stub edit". The stub gains a class and keeps its `#define`, so the block stays live and
    // the error inside it stays on screen.
    const std::string output =
        RunEditingOpenStub("#define SERVER_BUILD\nclass HostEntityA { void Spawn(); }\nclass HostEntityB {}\n",
                           /*expectNewFrame=*/false);

    INFO(PublishedFrames(output));

    const std::string last = LastPublishedFor(output, "main.as");
    INFO(last);
    REQUIRE_FALSE(last.empty());
    CHECK(last.find("as-err-undefined-identifier") != std::string::npos);
}

// =====================================================================================
// Script modules.
//
// `external shared class Foo;` says "this is built elsewhere", and elsewhere means another module.
// Measured, and stricter than it reads:
//
//     external shared class MExt;                        ERROR: External shared entity 'MExt' not found
//     external shared class MExt2; shared class MExt2 {} ERROR: External shared entity 'MExt2' not found
//
// The second one is the surprise. A full definition sitting in the SAME module does not satisfy an
// external declaration - so the question this rule has to ask is not "does this name exist", which
// the symbol table can answer, but "does it exist somewhere else", which it cannot.
//
// Without angelscript.modules configured the server cannot tell one module from another, so it asks
// the older, laxer question and accepts the false negative. A directory of scripts may be one module
// or one per file, and only the host knows which. These cases pin both answers, because a fix that
// only tightened the configured case would look identical to one that tightened everything.
// =====================================================================================

namespace
{
    /**
     * @brief A workspace with two modules: one that declares a shared class, one that externs it.
     *
     * The two are deliberately not connected by any `#include`. That is what makes them separate
     * modules, and it is the whole point of the fixture.
     */
    struct TwoModuleFixture
    {
        WorkspaceFixture fixture;

        TwoModuleFixture()
        {
            fixture.Write("mod_shared_lib.as", "shared class ModPacket { int id; }\n"
                                               "shared void ModHelper() { }\n");
            fixture.Write("mod_shared_main.as", "#include \"mod_shared_lib.as\"\n"
                                                "void ModSharedMain() { }\n");
        }

        std::string Entry(const char *name) const { return (fixture.dir / name).generic_string(); }
    };

    /**
     * @brief Opens `consumer.as` under a given module configuration and returns everything said.
     *
     * @param modules Empty means angelscript.modules is unset, which is the conservative default.
     */
    std::string RunWithModules(TwoModuleFixture &two,
                               const std::string &consumerSource,
                               const std::vector<config::ServerConfig::ModuleDefinition> &modules)
    {
        two.fixture.Write("mod_consumer.as", consumerSource);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(two.fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

        stream.Push(DidOpenMessage(two.fixture.Uri("mod_consumer.as"), consumerSource));
        stream.PushAction([&stream]() { WaitForCount(stream, "publishDiagnostics", 1); });

        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        serverConfig.modules = modules;

        // Not the default "none": the analyzer stays silent about a world it cannot see, and with an
        // empty table these assertions would pass by vacuity.
        serverConfig.engineProfile = "standard";

        RunScript(serverConfig, stream);
        return stream.Output();
    }

    std::vector<config::ServerConfig::ModuleDefinition> BothModules(const TwoModuleFixture &two)
    {
        return { { "shared", two.Entry("mod_shared_main.as") },
                 { "server", two.Entry("mod_consumer.as") } };
    }
}

TEST_CASE("Server - An external shared class is satisfied by another module")
{
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two, "external shared class ModPacket;\nvoid ModConsumerMain() { }\n", BothModules(two));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-external-not-found"));
}

TEST_CASE("Server - An external shared class is not satisfied by its own module")
{
    // The half that only module knowledge can catch, and today's false negative: the definition is
    // right there in the same file, the symbol table finds it, and the compiler rejects it anyway.
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two,
        "external shared class ModLocal;\nshared class ModLocal { int id; }\nvoid ModConsumerMain() { }\n",
        BothModules(two));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK(Published(output, "as-err-external-not-found"));
}

TEST_CASE("Server - An external shared function is satisfied by another module")
{
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two, "external shared void ModHelper();\nvoid ModConsumerMain() { }\n", BothModules(two));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-external-not-found"));
}

TEST_CASE("Server - Without configured modules the older, laxer question is asked")
{
    // The control, and it guards the thing that would actually hurt: tightening this rule for
    // everyone would report correct code as broken in every workspace that has not described its
    // modules, which is all of them by default.
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two,
        "external shared class ModLocal;\nshared class ModLocal { int id; }\nvoid ModConsumerMain() { }\n",
        {});

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-err-external-not-found"));
}

TEST_CASE("Server - A name no module declares shared is still reported")
{
    // The other control. A rule that had simply stopped firing would pass every case above.
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two, "external shared class ModNobodyHasThis;\nvoid ModConsumerMain() { }\n", BothModules(two));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK(Published(output, "as-err-external-not-found"));
}

TEST_CASE("Server - An import naming no configured module is a hint, never an error")
{
    // Measured: the compiler accepts `import void F() from "nevermind";` for a module that was never
    // built, because an imported function is bound at run time. So this may inform and must not
    // reject - the severity is the assertion here, not the presence.
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two,
        "import void ModImported() from \"nosuchmodule\";\nvoid ModConsumerMain() { ModImported(); }\n",
        BothModules(two));

    const std::string frames = PublishedFrames(output);
    INFO(frames);
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK(Published(output, "as-hint-import-unknown-module"));

    // Severity 4 is Hint in the protocol. If this ever becomes 1, the server is rejecting a script
    // the compiler accepts, and that is the assertion here - not that the diagnostic exists.
    //
    // Asked without assuming where "severity" sits inside the object. It used to look backwards
    // for a literal `{"severity"`, which passed on Windows and failed on Linux for no reason at
    // all: the two builds serialise the same object with its keys in a different order.
    CHECK(frames.find("\"severity\":4") != std::string::npos);

    // And nothing here is an error. This document publishes one diagnostic, so that pins the
    // severity exactly without having to find which object carries it.
    CHECK(frames.find("\"severity\":1") == std::string::npos);
}

TEST_CASE("Server - An import naming a configured module says nothing")
{
    // The control for the hint. Reporting every import would be noise on a correct project.
    TwoModuleFixture two;

    const std::string output = RunWithModules(
        two,
        "import void ModImported() from \"shared\";\nvoid ModConsumerMain() { ModImported(); }\n",
        BothModules(two));

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK_FALSE(Published(output, "as-hint-import-unknown-module"));
}

// =====================================================================================
// Folder modules, and diagnostics for files nobody opened.
//
// `scripts/maps` is the module MapScript and `scripts/plugins` is Plugin - a directory decides
// membership, which is how Sven Co-op lays its scripts out. A file belongs to exactly one module,
// most specific claim winning: an entry point's include closure, then the deepest folder, then any
// folder above it.
//
// The cases below are the ones PLANNED-FOLDER-MODULES.md says go wrong without care. Each is a row
// of that table.
// =====================================================================================

namespace
{
    /**
     * @brief A workspace laid out the way the design describes, modules and all.
     *
     * `scripts/maps/` holds a file with an error in it that nothing opens. That is the headline:
     * the error has to reach the client anyway.
     */
    struct FolderModuleFixture
    {
        WorkspaceFixture fixture;

        FolderModuleFixture()
        {
            fixture.Write("scripts/maps/broken_map.as",
                          "void FmBrokenMap()\n{\n    FmNoSuchFunction();\n}\n");
            fixture.Write("scripts/maps/clean_map.as", "void FmCleanMap() { }\n");
            fixture.Write("scripts/plugins/plugin_main.as", "void FmPluginMain() { }\n");
            fixture.Write("scripts/maps/deep/deep_map.as", "void FmDeepMap() { }\n");
        }

        std::string Dir(const char *name) const { return (fixture.dir / name).generic_string(); }
        std::string File(const char *name) const { return (fixture.dir / name).generic_string(); }
    };

    /** @brief Runs the fixture under a module configuration and returns everything the server said. */
    std::string RunWithFolderModules(FolderModuleFixture &fx,
                                     const std::vector<config::ServerConfig::ModuleDefinition> &modules,
                                     const char *openFile = "scripts/plugins/plugin_main.as")
    {
        const std::string source = "void FmOpenedDocument() { }\n";
        fx.fixture.Write("opened.as", source);

        test::ScriptedStream stream;
        stream.Push(InitializeWithProgress(fx.fixture.RootUri(), /*workDoneProgress=*/true));
        stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
        stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

        stream.Push(DidOpenMessage(fx.fixture.Uri(openFile), "void FmOpened() { }\n"));

        // The module pass schedules its members onto the analysis thread, which debounces. Waiting
        // for the file that is NOT open to be published is what makes this about the feature rather
        // than about the open document.
        stream.PushAction([&stream]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (stream.OutputContains("broken_map.as"))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });

        stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

        config::ServerConfig serverConfig;
        serverConfig.modules = modules;
        serverConfig.engineProfile = "standard";

        RunScript(serverConfig, stream);
        return stream.Output();
    }
}

TEST_CASE("Server - A folder module reports a file nobody opened")
{
    // The headline. Without a module, an error in scripts/maps/broken_map.as appears only once that
    // file is opened; the point of naming the folder is that it reaches the Problems panel first.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(fx, { { "MapScript", "", fx.Dir("scripts/maps") } });

    INFO(PublishedFrames(output));
    REQUIRE(output.find("publishDiagnostics") != std::string::npos);

    CHECK(CountPublishedFor(output, "broken_map.as") > 0);
    CHECK(LastPublishedFor(output, "broken_map.as").find("as-err-undefined-identifier") != std::string::npos);
}

TEST_CASE("Server - Without a module nothing is said about an unopened file")
{
    // The control, and the property to protect: with angelscript.modules empty this server behaves
    // exactly as it did, analysing the open document and nothing else. A feature that published for
    // every file in the workspace regardless would pass the case above.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(fx, {});

    INFO(PublishedFrames(output));
    CHECK(CountPublishedFor(output, "broken_map.as") == 0);
}

TEST_CASE("Server - The deepest folder wins when two claim one file")
{
    // `scripts/maps` and `scripts/maps/deep` both contain deep_map.as. The inner declaration is the
    // more specific statement, the way a nested .gitignore is.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(
        fx, { { "MapScript", "", fx.Dir("scripts/maps") },
              { "DeepMaps",  "", fx.Dir("scripts/maps/deep") } });

    const std::string published = LastPublishedFor(output, "deep_map.as");
    INFO(published);
    REQUIRE_FALSE(published.empty());

    // The hint names the module it was assigned to, and the one that lost.
    CHECK(published.find("as-hint-file-in-several-modules") != std::string::npos);
    CHECK(published.find("'DeepMaps'") != std::string::npos);
    CHECK(published.find("'MapScript'") != std::string::npos);
}

TEST_CASE("Server - A file claimed by only one module says nothing about it")
{
    // The control for the hint. Reporting every file in every module would be noise on a correct
    // project, and would make the hint useless exactly where it matters.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(fx, { { "MapScript", "", fx.Dir("scripts/maps") } });

    const std::string published = LastPublishedFor(output, "clean_map.as");
    INFO(published);
    REQUIRE_FALSE(published.empty());
    CHECK(published.find("as-hint-file-in-several-modules") == std::string::npos);
}

TEST_CASE("Server - An entry point beats a folder that also contains the file")
{
    // A folder module containing another module's entry point is legal and probably intentional.
    // Naming a file outright is the most specific claim there is.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(
        fx, { { "MapScript", "", fx.Dir("scripts/maps") },
              { "TheMap", fx.File("scripts/maps/clean_map.as"), "" } });

    const std::string published = LastPublishedFor(output, "clean_map.as");
    INFO(published);
    REQUIRE_FALSE(published.empty());

    CHECK(published.find("as-hint-file-in-several-modules") != std::string::npos);
    CHECK(published.find("'TheMap'") != std::string::npos);
}

TEST_CASE("Server - Two modules with the same name are refused, loudly")
{
    // "Declared in another module" stops having an answer when two modules share a name, so the
    // second is dropped - and said out loud, because a module that silently does not exist reads
    // exactly like a module whose rules found nothing to say.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(
        fx, { { "MapScript", "", fx.Dir("scripts/maps") },
              { "MapScript", "", fx.Dir("scripts/plugins") } });

    CHECK(output.find("two modules are both named") != std::string::npos);
}

TEST_CASE("Server - A module naming a folder that is not there is refused, loudly")
{
    // The silent-fallback failure this project has been bitten by twice: an empty module and a
    // mistyped path look identical from the outside.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(
        fx, { { "MapScript", "", fx.Dir("scripts/there-is-no-such-folder") } });

    CHECK(output.find("names a folder that does not exist") != std::string::npos);
}

TEST_CASE("Server - A module needs a name and at least one of a folder or an entry")
{
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(fx, { { "Nameless", "", "" } });

    CHECK(output.find("at least one of an entry ") != std::string::npos);
}

TEST_CASE("Server - An open document keeps its own analysis inside a module")
{
    // Two publishers for one URI is a race whose loser publishes an answer computed from older
    // text. The module pass skips anything the editor has open, so the buffer always wins.
    FolderModuleFixture fx;

    const std::string output = RunWithFolderModules(
        fx, { { "MapScript", "", fx.Dir("scripts/maps") } },
        /*openFile=*/"scripts/maps/clean_map.as");

    INFO(PublishedFrames(output));

    // Published once for the open document, by the open-document path. Not twice, and not with the
    // disk contents behind the buffer's back.
    const std::string published = LastPublishedFor(output, "clean_map.as");
    REQUIRE_FALSE(published.empty());
    CHECK(published.find("as-err-") == std::string::npos);
}

// =====================================================================================
// The three rows of PLANNED-FOLDER-MODULES.md that shipped without a test.
//
// Named as such in the report that went with them, which is the only reason they are here now:
// "not covered" is a claim with a shelf life, and this is it running out.
// =====================================================================================

TEST_CASE("Server - A renamed module folder is not remembered at its old path")
{
    // The cost the startup cache was documented as accepting: `weakly_canonical` per walked file was
    // 85% of startup, so the canonicalisation of each directory is remembered. A directory renamed
    // under a running server then keeps resolving to the path it had - and module membership, which
    // is decided by comparing paths, follows it there.
    //
    // That cost is only bounded if something clears the cache, and until this case nothing did.
    WorkspaceFixture fixture;
    fixture.Write("before/renamed_map.as", "void RnRenamedMap()\n{\n    RnNoSuchFunction();\n}\n");
    fixture.Write("opened.as", "void RnOpened() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    // The rename, and then the rescan a configuration change triggers. The module now names the new
    // path; nothing the server remembered about the old one may answer for it.
    stream.PushAction([&fixture]()
    {
        std::error_code ec;
        std::filesystem::rename(fixture.dir / "before", fixture.dir / "after", ec);
    });

    // A modules change, because that is the notification that now rebuilds the index - and the
    // folder this one names is the one the rename just created.
    stream.Push(R"({"jsonrpc":"2.0","method":"workspace/didChangeConfiguration","params":)"
                R"({"settings":{"angelscript":{"modules":[{"name":"Renamed","folder":")" +
                JsonEscape((fixture.dir / "after").generic_string()) + R"("}]}}}})");

    stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 2); });

    stream.Push(DidOpenMessage(fixture.Uri("opened.as"), "void RnOpened() { }\n"));

    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("renamed_map.as"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.engineProfile = "standard";
    // Deliberately none at startup. The module arrives with the configuration change below,
    // which is also what makes the change a change - re-sending the same definition is not one.

    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(PublishedFrames(output));

    // Found under its new path, which it can only be if the old canonicalisation was forgotten.
    REQUIRE(CountPublishedFor(output, "renamed_map.as") > 0);
    CHECK(LastPublishedFor(output, "renamed_map.as").find("as-err-undefined-identifier") != std::string::npos);

    // And the module was not reported as missing, which is what a stale path would have produced.
    CHECK(output.find("names a folder that does not exist") == std::string::npos);
}

TEST_CASE("Server - A symlink does not put one file in two modules twice over")
{
    // Asked of the filesystem rather than of the operating system: creating a symlink on Windows
    // needs Developer Mode or an elevated process, and a test that assumed either would fail on
    // whichever machine did not have it. The same discipline the case-sensitivity cases use.
    WorkspaceFixture fixture;
    fixture.Write("real/linked_map.as", "void SlLinkedMap() { }\n");
    fixture.Write("opened.as", "void SlOpened() { }\n");

    std::error_code linkError;
    std::filesystem::create_directory_symlink(fixture.dir / "real", fixture.dir / "mirror", linkError);

    if (linkError)
    {
        MESSAGE("symlinks are not available to this process; case skipped");
        return;
    }

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("opened.as"), "void SlOpened() { }\n"));
    stream.PushAction([&stream]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (stream.OutputContains("linked_map.as"))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.engineProfile = "standard";
    serverConfig.modules = { { "Real",   "", (fixture.dir / "real").generic_string() },
                             { "Mirror", "", (fixture.dir / "mirror").generic_string() } };

    RunScript(serverConfig, stream);

    const std::string output = stream.Output();
    INFO(PublishedFrames(output));

    // The walk resolves symlinks, so both module folders name the same directory and the file is
    // seen once. What must not happen is two publishers for it - the same race an open document in
    // a module would be, arriving by a different road.
    const size_t published = CountPublishedFor(output, "linked_map.as");
    INFO("published " << published << " time(s)");
    CHECK(published <= 1);
}

TEST_CASE("Server - The cost of a module-wide pass is measured rather than assumed")
{
    // The design accepts that a save re-analyses the whole module. Accepting a cost is not the same
    // as knowing it, and this is the number that was missing - printed rather than bounded, because
    // a threshold here would be a test about the machine the suite runs on.
    WorkspaceFixture fixture;

    constexpr int k_files = 200;
    for (int i = 0; i < k_files; ++i)
    {
        fixture.Write("module/file" + std::to_string(i) + ".as",
                      "void MpFunction" + std::to_string(i) + "() { int v = " + std::to_string(i) + "; }\n");
    }
    fixture.Write("opened.as", "void MpOpened() { }\n");

    test::ScriptedStream stream;
    stream.Push(InitializeWithProgress(fixture.RootUri(), /*workDoneProgress=*/true));
    stream.Push(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    stream.PushAction([&stream]() { WaitForCount(stream, "\"kind\":\"end\"", 1); });

    stream.Push(DidOpenMessage(fixture.Uri("opened.as"), "void MpOpened() { }\n"));

    std::chrono::steady_clock::time_point savedAt;
    stream.PushAction([&savedAt]() { savedAt = std::chrono::steady_clock::now(); });

    stream.Push(R"({"jsonrpc":"2.0","method":"textDocument/didSave","params":{"textDocument":{"uri":")" +
                fixture.Uri("opened.as") + R"("},"text":"void MpOpened() { }\n"}})");

    std::chrono::milliseconds elapsed{ 0 };
    stream.PushAction([&stream, &savedAt, &elapsed]()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (CountPublishedFor(stream.Output(), "module/file") >= k_files)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - savedAt);
    });

    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");

    config::ServerConfig serverConfig;
    serverConfig.engineProfile = "standard";
    serverConfig.modules = { { "Big", "", (fixture.dir / "module").generic_string() } };

    RunScript(serverConfig, stream);

    const size_t published = CountPublishedFor(stream.Output(), "module/file");
    MESSAGE("module-wide pass: " << published << " of " << k_files
                                 << " files published " << elapsed.count() << " ms after the save");

    // Every member reached the client. The timing above is the number worth having; this is the
    // assertion, because a pass that published half the module would still look fast.
    CHECK(published == static_cast<size_t>(k_files));
}
