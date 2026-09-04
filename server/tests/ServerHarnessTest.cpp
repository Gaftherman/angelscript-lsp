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
            std::ofstream out(dir / name, std::ios::binary);
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
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
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
    stream.Push(R"({"jsonrpc":"2.0","id":2,"method":"shutdown"})");
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
