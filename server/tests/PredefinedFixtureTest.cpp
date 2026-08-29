#include <doctest/doctest.h>

#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// =====================================================================================
// The full add-on stub, exercised.
//
// tests/fixtures/full-addons.as.predefined describes the AngelScript SDK's standard add-ons. Two
// things have to hold for it to be worth anything, and they are different claims:
//
//   1. The stub itself parses and collects cleanly. A stub that produces diagnostics is broken as a
//      stub - the user would see errors in a file they did not write - and it is the failure mode a
//      hand-written stub actually has, because nothing else ever compiles one.
//
//   2. Scripts written against it draw no diagnostics. This is the direction that matters: the
//      codebase's standing rule is that a missed error costs nothing and a false one costs the
//      user's trust in every other diagnostic on screen. Each case below is ordinary, correct usage
//      of one add-on, and every one of them must be silent.
//
// Where AS-Harness registers the same add-on, these scripts are also checked against the real
// compiler - see ParityAuditTest.cpp and the fixtures' PROVENANCE note.
// =====================================================================================

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    namespace fs = std::filesystem;

    std::string ReadFixture()
    {
        // The test binary is not run from a fixed directory, so the fixture is looked for relative
        // to a few plausible roots rather than assumed.
        static const char *candidates[] = {
            "tests/fixtures/full-addons.as.predefined",
            "../tests/fixtures/full-addons.as.predefined",
            "../../tests/fixtures/full-addons.as.predefined",
            "server/tests/fixtures/full-addons.as.predefined",
            "../server/tests/fixtures/full-addons.as.predefined",
            "../../server/tests/fixtures/full-addons.as.predefined",
        };

        for (const char *candidate : candidates)
        {
            std::ifstream in(candidate, std::ios::binary);
            if (in.is_open())
            {
                std::ostringstream ss;
                ss << in.rdbuf();
                return ss.str();
            }
        }
        return {};
    }

    const std::string &Fixture()
    {
        static const std::string contents = ReadFixture();
        return contents;
    }

    struct Result
    {
        std::vector<Diagnostic> stubDiagnostics;
        std::vector<Diagnostic> scriptDiagnostics;
    };

    /** @brief Loads the fixture as a stub, then analyses `script` against it. */
    Result Analyze(const std::string &script)
    {
        Result result;

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        angel_lsp::i18n::I18n i18n;
        angel_lsp::config::TypeConfig types;

        result.stubDiagnostics =
            collector.CollectSymbols("file:///full-addons.as.predefined", Fixture(), parser, table, &i18n, &types);

        const std::string uri = "file:///script.as";
        const auto scriptCollected = collector.CollectSymbols(uri, script, parser, table, &i18n, &types);
        result.scriptDiagnostics = scriptCollected;

        SemanticAnalysisRequest request{ table, uri, ".as.predefined", &i18n };
        request.typeConfig = &types;
        request.scopeRoot = scopes.CollectScopes(script, parser);
        request.sourceCode = script;
        request.tree = parser.Parse(script);

        SemanticAnalyzer analyzer(nullptr);
        for (auto &diagnostic : analyzer.Analyze(request))
        {
            result.scriptDiagnostics.push_back(std::move(diagnostic));
        }

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return result;
    }

    std::string Describe(const std::vector<Diagnostic> &diagnostics)
    {
        std::ostringstream ss;
        for (const auto &d : diagnostics)
        {
            ss << "\n  line " << (d.range.start.line + 1) << "  " << d.code << "  " << d.message;
        }
        return ss.str();
    }

    std::vector<Diagnostic> ErrorsOnly(const std::vector<Diagnostic> &diagnostics)
    {
        std::vector<Diagnostic> errors;
        for (const auto &d : diagnostics)
        {
            if (d.severity == DiagnosticSeverity::Error)
            {
                errors.push_back(d);
            }
        }
        return errors;
    }

    /**
     * @brief Asserts the script draws no errors, and says what it drew when it does.
     *
     * Errors rather than every diagnostic: several of these scripts declare a variable purely to
     * prove its type resolves, and "never used" is then a correct warning about the test rather
     * than a finding about the stub. The false positives this file exists to catch are errors.
     */
    void RequireSilent(const std::string &script)
    {
        const Result result = Analyze(script);
        const auto errors = ErrorsOnly(result.scriptDiagnostics);
        INFO("stub diagnostics:" << Describe(result.stubDiagnostics));
        INFO("script errors:" << Describe(errors));
        CHECK(errors.empty());
    }
}

TEST_CASE("PredefinedFixture - The stub is present and parses without diagnostics")
{
    REQUIRE_MESSAGE(!Fixture().empty(),
                    "tests/fixtures/full-addons.as.predefined not found from the working directory");

    const Result result = Analyze("void main() {}\n");
    const auto errors = ErrorsOnly(result.stubDiagnostics);
    INFO("stub diagnostics:" << Describe(result.stubDiagnostics));
    CHECK(errors.empty());
}

TEST_CASE("PredefinedFixture - Every add-on it declares resolves")
{
    // One declaration per add-on, which is the cheapest possible proof that the type reached the
    // symbol table under the name the stub spells it with.
    RequireSilent(
        "void main()\n"
        "{\n"
        "    string s;\n"
        "    array<int> a;\n"
        "    grid<int> g;\n"
        "    dictionary d;\n"
        "    ref r;\n"
        "    any v;\n"
        "    datetime t;\n"
        "    file f;\n"
        "    filesystem fsys;\n"
        "    complex c;\n"
        "    socket sock;\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - string")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    string s = \"hello\";\n"
        "    s += \" world\";\n"
        "    uint n = s.length();\n"
        "    string part = s.substr(0, 5);\n"
        "    int at = s.findFirst(\"o\");\n"
        "    s.insert(0, \">> \");\n"
        "    s.erase(0, 3);\n"
        "    array<string>@ words = s.split(\" \");\n"
        "    println(join(words, \", \"));\n"
        "    println(formatInt(42, \"\", 8) + formatFloat(3.5, \"\", 0, 2));\n"
        "    int64 parsed = parseInt(\"123\");\n"
        "    println(\"\" + parsed + n + at + part.length());\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - array and grid")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    array<int> numbers = {3, 1, 2};\n"
        "    numbers.insertLast(4);\n"
        "    numbers.sortAsc();\n"
        "    numbers.reverse();\n"
        "    int first = numbers[0];\n"
        "    int found = numbers.find(2);\n"
        "    array<array<int>> nested = {{1, 2}, {3, 4}};\n"
        "    array<int> sized(10, 0);\n"
        "    grid<int> board = {{1, 2}, {3, 4}};\n"
        "    board.resize(4, 4);\n"
        "    int cell = board[1, 1];\n"
        "    println(\"\" + first + found + nested.length() + sized.length() + cell + board.width());\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - dictionary")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    dictionary d = {{'name', 'value'}, {'count', 3}};\n"
        "    d.set(\"key\", 95);\n"
        // Named `stored`, not `out`: `out` is a reserved keyword, and the real compiler answers it
        // with "Expected '(' | Instead found reserved keyword 'out'". Found by running this very
        // script through asharness.
        "    int64 stored = 0;\n"
        "    if (d.get(\"key\", stored)) { println(\"\" + stored); }\n"
        "    if (d.exists(\"name\")) { d.delete(\"name\"); }\n"
        "    array<string>@ keys = d.getKeys();\n"
        "    println(\"\" + keys.length() + d.getSize());\n"
        "    d.deleteAll();\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - ref, any and weakref")
{
    RequireSilent(
        "class Node { int value; }\n"
        "void main()\n"
        "{\n"
        "    Node@ node = Node();\n"
        // Constructed, not assigned: `ref` declares opHndlAssign and no opAssign, so `ref r = @node;`
        // is rejected with "No appropriate opAssign method found in 'ref' for value assignment".
        "    ref generic(@node);\n"
        "    any boxed;\n"
        "    boxed.store(@node);\n"
        "    weakref<Node> weak(node);\n"
        "    Node@ resolved = weak.get();\n"
        "    if (resolved !is null) { println(\"\" + resolved.value); }\n"
        "    const_weakref<Node> constWeak(node);\n"
        "    println(\"\" + (constWeak.get() is null));\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - datetime")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    datetime now;\n"
        "    datetime birthday(2000, 1, 1);\n"
        "    int64 elapsed = now - birthday;\n"
        "    datetime later = now + 3600;\n"
        "    now.setTime(12, 0, 0);\n"
        "    println(\"\" + now.get_year() + later.get_hour() + elapsed);\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - file and filesystem")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    file f;\n"
        "    if (f.open(\"data.txt\", \"r\") >= 0)\n"
        "    {\n"
        "        while (!f.isEndOfFile()) { println(f.readLine()); }\n"
        "        f.close();\n"
        "    }\n"
        "    filesystem fsys;\n"
        "    fsys.changeCurrentPath(\"/tmp\");\n"
        "    array<string>@ files = fsys.getFiles();\n"
        "    for (uint i = 0; i < files.length(); i++)\n"
        "    {\n"
        "        if (!fsys.isDir(files[i])) { println(\"\" + fsys.getSize(files[i])); }\n"
        "    }\n"
        "    datetime modified = fsys.getModifyDateTime(\"data.txt\");\n"
        "    println(\"\" + modified.get_day());\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - exception handling")
{
    RequireSilent(
        "void risky()\n"
        "{\n"
        "    throw(\"something went wrong\");\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    try\n"
        "    {\n"
        "        risky();\n"
        "    }\n"
        "    catch\n"
        "    {\n"
        "        println(getExceptionInfo());\n"
        "    }\n"
        "    dictionary context;\n"
        "    context.set(\"where\", \"main\");\n"
        "    SetException(\"raised by hand\", context);\n"
        "    Exception@ current = GetException();\n"
        "    if (current !is null) { println(current.message + current.func + current.line); }\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - co-routines")
{
    RequireSilent(
        "void worker(dictionary@ args)\n"
        "{\n"
        "    int64 count = 0;\n"
        "    args.get(\"count\", count);\n"
        "    for (int64 i = 0; i < count; i++)\n"
        "    {\n"
        "        println(\"tick \" + i);\n"
        "        yield();\n"
        "    }\n"
        "}\n"
        "void main()\n"
        "{\n"
        "    dictionary@ args = dictionary();\n"
        "    args.set(\"count\", 3);\n"
        "    createCoRoutine(@worker, args);\n"
        "    sleep(100);\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - complex and math")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    complex a(1, 2);\n"
        "    complex b = {3, 4};\n"
        "    complex sum = a + b;\n"
        "    float magnitude = sum.abs();\n"
        "    float angle = atan2(sum.i, sum.r);\n"
        "    println(\"\" + magnitude + angle + sqrt(2.0f) + abs(-1.0f) + pow(2.0f, 8.0f));\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - socket")
{
    // The real add-on's API, from sdk/add_on/scriptsocket/scriptsocket.cpp. An earlier version of
    // this fixture invented a larger one - bind/available/getLastError, address-family enums, a host
    // string for connect - on the mistaken belief that the SDK ships no socket add-on. It does, and
    // `connect` takes a packed IPv4 address rather than a name.
    RequireSilent(
        "void main()\n"
        "{\n"
        "    socket client;\n"
        "    if (client.connect(2130706433, 8080) >= 0)\n"
        "    {\n"
        "        client.send(\"ping\");\n"
        "        println(client.receive(1000));\n"
        "        client.close();\n"
        "    }\n"
        "    println(\"\" + client.isActive());\n"
        "}\n");
}

TEST_CASE("PredefinedFixture - socket accepts connections")
{
    RequireSilent(
        "void main()\n"
        "{\n"
        "    socket server;\n"
        "    if (server.listen(8080) >= 0)\n"
        "    {\n"
        "        socket\n client = server.accept(500);\n"
        "        if (client !is null) { client.send(\"hello\"); client.close(); }\n"
        "    }\n"
        "    server.close();\n"
        "}\n");
}

// =====================================================================================
// The list patterns the stub carries are load-bearing, so they are asserted rather than assumed.
// Each expectation is the real compiler's, for the add-on whose registration the tag was copied
// from - see the fixture's own notes.
// =====================================================================================

TEST_CASE("PredefinedFixture - The declared list patterns are enforced")
{
    const auto errorsIn = [](const std::string &script)
    {
        const Result result = Analyze(script);
        std::vector<std::string> codes;
        for (const auto &d : result.scriptDiagnostics)
        {
            if (d.code == "as-err-initializer-list-not-supported" ||
                d.code == "as-err-initializer-list-expected")
            {
                codes.push_back(d.code);
            }
        }
        return codes;
    };

    // array<T> is `{repeat T}`: a brace where an `int` belongs is wrong.
    CHECK(errorsIn("void main() { array<int> a = {1, {2}}; }\n").size() == 1);

    // dictionary is `{repeat {string, ?}}`: elements must be pairs...
    CHECK(errorsIn("void main() { dictionary d = {1, 2}; }\n").size() == 2);
    // ...and `?` takes a value, not a list.
    CHECK(errorsIn("void main() { dictionary d = {{'a', {1}}}; }\n").size() == 1);
    CHECK(errorsIn("void main() { dictionary d = {{'a', 1}}; }\n").empty());

    // grid<T> is `{repeat {repeat_same T}}`: rows of elements.
    CHECK(errorsIn("void main() { grid<int> g = {{1, 2}, {3, 4}}; }\n").empty());
    CHECK(errorsIn("void main() { grid<int> g = {{1, {2}}}; }\n").size() == 1);

    // complex is `{float, float}` - a fixed pair, not a repeat.
    CHECK(errorsIn("void main() { complex c = {1, 2}; }\n").empty());
    CHECK(errorsIn("void main() { complex c = {{1}, 2}; }\n").size() == 1);
}
