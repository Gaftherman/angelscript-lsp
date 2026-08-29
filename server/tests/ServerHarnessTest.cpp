#include <doctest/doctest.h>

#include "helpers/ScriptedStream.h"
#include "lsp/Server.h"

#include <algorithm>
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
        static const std::string k_marker = R"("method":"textDocument/publishDiagnostics")";
        std::string frames;

        for (size_t at = output.find(k_marker); at != std::string::npos; at = output.find(k_marker, at + 1))
        {
            const size_t end = output.find("Content-Length:", at);
            frames += output.substr(at, end == std::string::npos ? std::string::npos : end - at);
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
