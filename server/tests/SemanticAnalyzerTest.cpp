#include <doctest/doctest.h>

#include "helpers/CorpusDirectory.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /** @brief Runs the full parse -> SymbolCollector -> LocalScopeCollector -> SemanticAnalyzer
     *         pipeline for sourceCode and returns the analyzer's diagnostics. table and i18n are
     *         caller-owned since SemanticAnalysisRequest only holds a reference/pointer to them. */
    std::vector<Diagnostic> AnalyzeSource(const std::string &sourceCode, SymbolTable &table,
                                           const angel_lsp::i18n::I18n &i18n, const std::string &fileUri = "file:///test.as")
    {
        AngelScriptParser symbolParser;
        SymbolCollector symbolCollector(nullptr);
        symbolCollector.CollectSymbols(fileUri, sourceCode, symbolParser, table);

        AngelScriptParser scopeParser;
        LocalScopeCollector scopeCollector(nullptr);

        SemanticAnalysisRequest req{table, fileUri, "", &i18n};
        req.scopeRoot = scopeCollector.CollectScopes(sourceCode, scopeParser);

        // The server always analyses with the source text and the parsed tree in hand
        // (Server.cpp: AnalyzeDocument). Leaving them null here made this harness model something
        // the server never is, and any rule that has to look at the syntax to decide - the
        // base-constructor `super(...)` exemption, for one - silently took its no-tree branch, so
        // the test passed without exercising the rule.
        AngelScriptParser treeParser;
        req.sourceCode = sourceCode;
        req.tree = treeParser.Parse(sourceCode);

        SemanticAnalyzer analyzer(nullptr);
        auto diagnostics = analyzer.Analyze(req);

        if (req.tree)
            ts_tree_delete(const_cast<TSTree *>(req.tree));

        return diagnostics;
    }

    /** @brief True if diagnostics contains an as-err-undeclared-identifier flagging exactly name. */
    bool HasUndefinedIdentifierDiagnostic(const std::vector<Diagnostic> &diagnostics, const std::string &name)
    {
        std::string quoted = "'" + name + "'";
        for (const auto &diag : diagnostics)
        {
            if (diag.code == "as-err-undeclared-identifier" && diag.message.find(quoted) != std::string::npos)
                return true;
        }
        return false;
    }

    /** @brief True if diagnostics contains an as-warn-unused-variable flagging exactly name. */
    bool HasUnusedVariableDiagnostic(const std::vector<Diagnostic> &diagnostics, const std::string &name)
    {
        std::string quoted = "'" + name + "'";
        for (const auto &diag : diagnostics)
        {
            if (diag.code == "as-warn-unused-variable" && diag.message.find(quoted) != std::string::npos)
                return true;
        }
        return false;
    }

    /** @brief Counts as-warn-unused-variable diagnostics flagging exactly name. */
    size_t CountUnusedVariableDiagnostics(const std::vector<Diagnostic> &diagnostics, const std::string &name)
    {
        std::string quoted = "'" + name + "'";
        size_t count = 0;
        for (const auto &diag : diagnostics)
        {
            if (diag.code == "as-warn-unused-variable" && diag.message.find(quoted) != std::string::npos)
                ++count;
        }
        return count;
    }

    /** @brief True if diagnostics contains an as-err-null-non-handle flagging exactly typeName. */
    bool HasNullNonHandleDiagnostic(const std::vector<Diagnostic> &diagnostics, const std::string &typeName)
    {
        std::string quoted = "'" + typeName + "'";
        for (const auto &diag : diagnostics)
        {
            if (diag.code == "as-err-null-non-handle" && diag.message.find(quoted) != std::string::npos)
                return true;
        }
        return false;
    }

    /** @brief Extracts the single-quoted identifier name out of an as-err-undeclared-identifier message. */
    std::string ExtractFlaggedName(const std::string &message)
    {
        size_t open = message.find('\'');
        size_t close = (open == std::string::npos) ? std::string::npos : message.find('\'', open + 1);
        if (open == std::string::npos || close == std::string::npos)
            return message;
        return message.substr(open + 1, close - open - 1);
    }

    /** @brief Reads an entire file from the angelscript/ corpus into memory; empty string if missing. */
    std::string ReadCorpusFile(const std::string &fileName)
    {
        std::string path = angel_lsp::test::CorpusDirectory().string() + "/" + fileName;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return "";

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

// =====================================================================================
// Undefined-identifier detection
// =====================================================================================

TEST_CASE("SemanticAnalyzer - genuinely undefined identifier is flagged")
{
    std::string source = R"AS(
void Foo()
{
    Undefined = 5;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "Undefined"));

    for (const auto &diag : diagnostics)
    {
        if (diag.code == "as-err-undeclared-identifier")
            CHECK(diag.severity == DiagnosticSeverity::Warning);
    }
}

TEST_CASE("SemanticAnalyzer - locally-defined variable reference is not flagged")
{
    std::string source = R"AS(
void Foo()
{
    int x = 1;
    x = x + 1;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "x"));
}

TEST_CASE("SemanticAnalyzer - reference to a globally-declared function is not flagged")
{
    std::string source = R"AS(
void Helper()
{
}

void Foo()
{
    Helper();
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "Helper"));
}

TEST_CASE("SemanticAnalyzer - engine globals declared in a predefined-style file are not flagged")
{
    // Mirrors Server::ParserPredefined's actual sequence: a .as.predefined-style source is
    // collected into the shared SymbolTable with no scope tree (predefined files are
    // declarations only - ParserPredefined never calls LocalScopeCollector), then a real
    // script sharing that table is analyzed. Names are intentionally generic/fictional
    // (Entity/Engine), not a claim about the real SvenCoop API surface - this proves the
    // mechanism the corpus audit's "self"/"g_Engine" false positives are missing, not a
    // real stub library (see the "Undefined Identifier Corpus Audit" test's MESSAGE for why
    // the corpus itself has no .as.predefined file for this to load in that audit).
    std::string predefinedSource = R"AS(
class Entity
{
    void Log(const string &in msg) {}
}

class Engine
{
    void Log(const string &in msg) {}
}

Entity@ self;
Engine@ g_Engine;
)AS";

    std::string scriptSource = R"AS(
void Foo()
{
    self.Log("hi");
    g_Engine.Log("hi");
    Undefined = 5;
}
)AS";

    SymbolTable table;
    AngelScriptParser predefinedParser;
    SymbolCollector symbolCollector(nullptr);
    symbolCollector.CollectSymbols("file:///stubs.as.predefined", predefinedSource, predefinedParser, table);

    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(scriptSource, table, i18n, "file:///script.as");

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "self"));
    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "g_Engine"));
    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "Undefined"));
}

TEST_CASE("SemanticAnalyzer - member access is not flagged, but the undefined object still is")
{
    std::string source = R"AS(
void Foo()
{
    obj.value = 5;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "value"));
    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "obj"));
}

TEST_CASE("SemanticAnalyzer - this is never flagged as undefined")
{
    // "this" is now a dedicated this_expression node in tree-sitter-angelscript (fixed upstream
    // after a full-corpus audit found 1,962 false positives - "this" used to be a plain identifier
    // node, indistinguishable from any other name, so LOCALS_QUERY's bare (identifier)
    // @local.reference matched it and this check flagged every legitimate "this.Foo()" as
    // undefined). CheckUndefinedIdentifiers has no "this" special-case anymore; this test now
    // guards the grammar fix itself - it would catch a regression to an older grammar commit.
    // ("self", found alongside "this" in the same audit with 24,961 false positives, is a
    // different kind of gap: a genuine SvenCoop-engine-injected global with no AngelScript
    // language meaning, so it correctly stays unresolved and is NOT exempted here.)
    std::string source = R"AS(
class Foo
{
    void Bar()
    {
        this.Bar();
    }
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "this"));
}

// =====================================================================================
// Unused local variable
// =====================================================================================

TEST_CASE("SemanticAnalyzer - a genuinely unused local variable is flagged")
{
    std::string source = R"AS(
void Foo()
{
    int unused = 5;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "unused"));

    for (const auto &diag : diagnostics)
    {
        if (diag.code == "as-warn-unused-variable")
            CHECK(diag.severity == DiagnosticSeverity::Warning);
    }
}

TEST_CASE("SemanticAnalyzer - an unreferenced module-level global is not flagged")
{
    // Found via corpus spot-check: LOCALS_QUERY's @local.definition.var captures both true
    // locals and globals under the identical LocalDefinitionKind::Variable (its own comment
    // says "Variables (locals and globals)"). "Unused" isn't decidable for a global from a
    // single file's Scope tree alone - another file in the same workspace, or the engine
    // itself, may reference it. Matches the real "string WPN_NAME = ...;" pattern found never
    // referenced anywhere in its own file.
    std::string source = R"AS(
string WPN_NAME = "AK-47";
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "WPN_NAME"));
}

TEST_CASE("SemanticAnalyzer - an unreferenced namespace-scope global is not flagged")
{
    std::string source = R"AS(
namespace Config
{
    bool USE_ZONES = false;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "USE_ZONES"));
}

TEST_CASE("SemanticAnalyzer - a variable that is read is not flagged")
{
    std::string source = R"AS(
void Foo()
{
    int x = 1;
    x = x + 1;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "x"));
}

TEST_CASE("SemanticAnalyzer - a variable that is only assigned to, never read, is still not flagged")
{
    // Deliberate scope decision: LocalReference doesn't distinguish a read from an assignment
    // target, so this check answers "is this name referenced anywhere at all," not the
    // stricter "is this value ever read after being assigned" (a separate, unimplemented
    // dead-store check).
    std::string source = R"AS(
void Foo()
{
    int x;
    x = 5;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "x"));
}

TEST_CASE("SemanticAnalyzer - shadowing: only the genuinely-unused outer variable is flagged")
{
    std::string source = R"AS(
void Foo()
{
    int i = 0;
    {
        int i = 1;
        i = i + 1;
    }
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    // Exactly one "i" is unused (the outer one) - if resolution weren't shadowing-aware
    // (matching by name instead of ResolveInScope), this would come out as 0 (inner use
    // wrongly satisfies both) or 2 (inner definition wrongly counted unused too).
    CHECK(CountUnusedVariableDiagnostics(diagnostics, "i") == 1);
}

TEST_CASE("SemanticAnalyzer - an unused function parameter is not flagged")
{
    std::string source = R"AS(
void Foo(int unusedParam)
{
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "unusedParam"));
}

TEST_CASE("SemanticAnalyzer - a class field is never considered by the unused-variable check")
{
    // "value" is never referenced as a bare identifier - only through this.value, which marks
    // the member side isMemberAccess and excludes it from local-reference resolution entirely.
    // Also exercises the LocalScopeCollector dedup fix indirectly: without it, "value" would
    // additionally exist as a spurious LocalDefinitionKind::Variable duplicate in class_body
    // scope with no local reference anywhere, and would very likely have been (wrongly) flagged.
    std::string source = R"AS(
class Foo
{
    int value;

    void SetValue()
    {
        this.value = 5;
    }
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "value"));
}

// =====================================================================================
// null-assigned-to-non-handle detection (module-scope globals and class-body fields only -
// function-body locals are covered separately once LocalScopeCollector carries type info)
// =====================================================================================

TEST_CASE("SemanticAnalyzer - a non-handle global initialized with null is flagged")
{
    std::string source = "int f = null;\n";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

TEST_CASE("SemanticAnalyzer - a non-handle class field initialized with null is flagged")
{
    std::string source = R"AS(
class Foo
{
    int count = null;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

TEST_CASE("SemanticAnalyzer - a non-handle CLASS type initialized with null is not flagged")
{
    // Unlike a VM-intrinsic primitive, a class/object type's null-compatibility depends on how
    // the engine registers it (a converting constructor or opAssign(int) could legitimately
    // accept null) - invisible to this analyzer without a predefined stub. Confirmed by the full
    // corpus audit: every real flagged occurrence before this exclusion was a SvenCoop "EHandle"
    // field ("EHandle eView = null;"), a genuine registered value type with exactly that pattern
    // (constructed via "EHandle(pView)" elsewhere in the same corpus) - not a real bug.
    std::string source = R"AS(
class Foo
{
    EHandle target = null;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "EHandle"));
}

TEST_CASE("SemanticAnalyzer - a handle initialized with null is not flagged")
{
    std::string source = "Foo@ h = null;\n";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "Foo@"));
}

TEST_CASE("SemanticAnalyzer - null used only as a call argument is not flagged")
{
    std::string source = "int f = someFunc(null);\n";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

TEST_CASE("SemanticAnalyzer - a non-null initializer is not flagged")
{
    std::string source = "int f = 5;\n";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

TEST_CASE("SemanticAnalyzer - null-non-handle check only fires for the analyzed file")
{
    // Two independent files sharing one SymbolTable - flagging must not leak across fileUri.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n;

    AngelScriptParser parserA;
    SymbolCollector collectorA(nullptr);
    collectorA.CollectSymbols("file:///a.as", "int f = null;\n", parserA, table);

    auto diagnosticsB = AnalyzeSource("int g = 5;\n", table, i18n, "file:///b.as");
    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnosticsB, "int"));
}

TEST_CASE("SemanticAnalyzer - a function-body local initialized with null is flagged")
{
    std::string source = R"AS(
void Foo()
{
    int f = null;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

TEST_CASE("SemanticAnalyzer - a function-body local handle initialized with null is not flagged")
{
    std::string source = R"AS(
void Foo()
{
    Bar@ h = null;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "Bar@"));
}

TEST_CASE("SemanticAnalyzer - a function-body local CLASS type initialized with null is not flagged")
{
    // Same reasoning as the module/class-field exclusion: a class type's null-compatibility
    // depends on engine registration, invisible to this analyzer.
    std::string source = R"AS(
void Foo()
{
    EHandle target = null;
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "EHandle"));
}

TEST_CASE("SemanticAnalyzer - null used only as a call argument in a function body is not flagged")
{
    std::string source = R"AS(
void Foo()
{
    int f = someFunc(null);
}
)AS";

    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    auto diagnostics = AnalyzeSource(source, table, i18n);

    CHECK_FALSE(HasNullNonHandleDiagnostic(diagnostics, "int"));
}

// =====================================================================================
// Full-corpus false-positive-rate audit (opt-in - skipped by default, run via
// `angel_lsp_tests.exe --no-skip --test-case="*Undefined Identifier Corpus Audit*"`)
// =====================================================================================

TEST_CASE("SemanticAnalyzer - Undefined Identifier Corpus Audit Across All angelscript Files" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(angel_lsp::test::CorpusDirectory()))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".as")
            files.push_back(entry.path());
    }
    REQUIRE_MESSAGE(!files.empty(), "Expected the angelscript/ corpus directory to contain .as files");
    std::sort(files.begin(), files.end());

    angel_lsp::i18n::I18n i18n;
    size_t totalFiles = 0;
    size_t totalFlagged = 0;
    double totalSeconds = 0.0;
    std::vector<std::string> sample;

    for (const auto &path : files)
    {
        std::string sourceCode = ReadCorpusFile(path.filename().string());
        if (sourceCode.empty())
            continue;

        ++totalFiles;

        SymbolTable table;
        std::vector<Diagnostic> diagnostics;
        auto start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(diagnostics = AnalyzeSource(sourceCode, table, i18n, "file:///" + path.filename().string()));
        totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        bool sampledThisFile = false;
        for (const auto &diag : diagnostics)
        {
            if (diag.code != "as-err-undeclared-identifier")
                continue;

            ++totalFlagged;
            // One example per file (not per diagnostic) so the sample spans many files instead
            // of exhausting the cap on whichever file happens to be flagged the most.
            if (!sampledThisFile && sample.size() < 60)
            {
                sample.push_back(path.filename().string() + ":" + std::to_string(diag.range.start.line + 1)
                                  + " " + diag.message);
                sampledThisFile = true;
            }
        }
    }

    MESSAGE("Undefined-identifier corpus audit: files=" << totalFiles
            << " totalFlagged=" << totalFlagged
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));

    for (const auto &line : sample)
        MESSAGE("  " << line);

    CHECK(totalFiles > 0);
}

// =====================================================================================
// Unused-variable corpus audit (opt-in - skipped by default, run via
// `angel_lsp_tests.exe --no-skip --test-case="*Unused Variable Corpus Audit*"`)
//
// Unlike undefined-identifier, "unused" is a purely per-file/per-scope property - it doesn't
// depend on cross-file or predefined-stub symbol visibility, so a single per-file pass (no
// grouped-workspace variant needed) is the right shape here.
// =====================================================================================

TEST_CASE("SemanticAnalyzer - Unused Variable Corpus Audit Across All angelscript Files" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(angel_lsp::test::CorpusDirectory()))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".as")
            files.push_back(entry.path());
    }
    REQUIRE_MESSAGE(!files.empty(), "Expected the angelscript/ corpus directory to contain .as files");
    std::sort(files.begin(), files.end());

    angel_lsp::i18n::I18n i18n;
    size_t totalFiles = 0;
    size_t totalFlagged = 0;
    double totalSeconds = 0.0;
    std::vector<std::string> sample;

    for (const auto &path : files)
    {
        std::string sourceCode = ReadCorpusFile(path.filename().string());
        if (sourceCode.empty())
            continue;

        ++totalFiles;

        SymbolTable table;
        std::vector<Diagnostic> diagnostics;
        auto start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(diagnostics = AnalyzeSource(sourceCode, table, i18n, "file:///" + path.filename().string()));
        totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        bool sampledThisFile = false;
        for (const auto &diag : diagnostics)
        {
            if (diag.code != "as-warn-unused-variable")
                continue;

            ++totalFlagged;
            if (!sampledThisFile && sample.size() < 60)
            {
                sample.push_back(path.filename().string() + ":" + std::to_string(diag.range.start.line + 1)
                                  + " " + diag.message);
                sampledThisFile = true;
            }
        }
    }

    MESSAGE("Unused-variable corpus audit: files=" << totalFiles
            << " totalFlagged=" << totalFlagged
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));

    for (const auto &line : sample)
        MESSAGE("  " << line);

    CHECK(totalFiles > 0);
}

// =====================================================================================
// Null-non-handle corpus audit (opt-in - skipped by default, run via
// `angel_lsp_tests.exe --no-skip --test-case="*Null Non Handle Corpus Audit*"`)
//
// Like unused-variable, "null assigned to a non-handle type" is a purely per-file/per-scope
// property (it only inspects the variable's own declaration), so a single per-file pass is
// the right shape - no grouped-workspace variant needed.
// =====================================================================================

TEST_CASE("SemanticAnalyzer - Null Non Handle Corpus Audit Across All angelscript Files" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(angel_lsp::test::CorpusDirectory()))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".as")
            files.push_back(entry.path());
    }
    REQUIRE_MESSAGE(!files.empty(), "Expected the angelscript/ corpus directory to contain .as files");
    std::sort(files.begin(), files.end());

    angel_lsp::i18n::I18n i18n;
    size_t totalFiles = 0;
    size_t totalFlagged = 0;
    double totalSeconds = 0.0;
    std::vector<std::string> sample;

    for (const auto &path : files)
    {
        std::string sourceCode = ReadCorpusFile(path.filename().string());
        if (sourceCode.empty())
            continue;

        ++totalFiles;

        SymbolTable table;
        std::vector<Diagnostic> diagnostics;
        auto start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(diagnostics = AnalyzeSource(sourceCode, table, i18n, "file:///" + path.filename().string()));
        totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        bool sampledThisFile = false;
        for (const auto &diag : diagnostics)
        {
            if (diag.code != "as-err-null-non-handle")
                continue;

            ++totalFlagged;
            if (!sampledThisFile && sample.size() < 60)
            {
                sample.push_back(path.filename().string() + ":" + std::to_string(diag.range.start.line + 1)
                                  + " " + diag.message);
                sampledThisFile = true;
            }
        }
    }

    MESSAGE("Null-non-handle corpus audit: files=" << totalFiles
            << " totalFlagged=" << totalFlagged
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));

    for (const auto &line : sample)
        MESSAGE("  " << line);

    CHECK(totalFiles > 0);
}

// =====================================================================================
// Grouped-workspace false-positive audit (opt-in - skipped by default, run via
// `angel_lsp_tests.exe --no-skip --test-case="*Grouped*"`)
//
// The per-file audit above analyzes every file in total isolation - a fresh, empty
// SymbolTable per file, no other workspace files visible, no predefined/engine stub
// declarations loaded. That's NOT how Server.cpp actually runs: it keeps one shared
// SymbolTable for the whole session, so a class declared in one file of a project and
// referenced from a sibling file is resolved correctly in production but would show up as
// "undefined" in the per-file audit. This test corrects that specific gap - it groups
// corpus files by filename prefix (a reasonable proxy for "files that came from the same
// mod/project", since these filenames are flattened directory paths) and shares one
// SymbolTable across every file in a group before checking any of them, exactly mirroring
// Server.cpp's actual collection order. It does NOT and cannot fix the other gap: this
// corpus has no predefined engine-stub files at all (confirmed - no file anywhere declares
// `string`, `dictionary`, `CBasePlayer`, or `g_Module`), so references to genuine external
// engine API surface will still be flagged here exactly as they would in an unconfigured
// real workspace. Comparing this run's totalFlagged against the per-file audit's isolates
// how much of the noise was purely a test-harness artifact vs. a real, structural blind spot.
// =====================================================================================

TEST_CASE("SemanticAnalyzer - Undefined Identifier Corpus Audit Grouped By Project (shared SymbolTable)" * doctest::skip(true))
{
    if (!angel_lsp::test::CorpusIsAvailable())
    {
        MESSAGE(angel_lsp::test::CorpusMissingMessage());
        return;
    }

    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(angel_lsp::test::CorpusDirectory()))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".as")
            files.push_back(entry.path());
    }
    REQUIRE_MESSAGE(!files.empty(), "Expected the angelscript/ corpus directory to contain .as files");
    std::sort(files.begin(), files.end());

    std::unordered_map<std::string, std::vector<fs::path>> groups;
    for (const auto &path : files)
    {
        std::string name = path.filename().string();
        size_t underscorePos = name.find('_');
        std::string key = (underscorePos == std::string::npos) ? name : name.substr(0, underscorePos);
        groups[key].push_back(path);
    }

    angel_lsp::i18n::I18n i18n;
    size_t totalFiles = 0;
    size_t totalFlagged = 0;
    double totalSeconds = 0.0;
    std::unordered_map<std::string, size_t> flaggedNameCounts;

    for (auto &[groupName, groupFiles] : groups)
    {
        SymbolTable sharedTable;
        std::unordered_map<std::string, std::string> sources;

        for (const auto &path : groupFiles)
        {
            std::string sourceCode = ReadCorpusFile(path.filename().string());
            if (sourceCode.empty())
                continue;

            std::string fileUri = "file:///" + path.filename().string();
            sources[fileUri] = sourceCode;

            AngelScriptParser parser;
            SymbolCollector collector(nullptr);
            collector.CollectSymbols(fileUri, sourceCode, parser, sharedTable);
        }

        for (const auto &[fileUri, sourceCode] : sources)
        {
            ++totalFiles;

            AngelScriptParser scopeParser;
            LocalScopeCollector scopeCollector(nullptr);

            SemanticAnalysisRequest req{sharedTable, fileUri, "", &i18n};

            auto start = std::chrono::steady_clock::now();
            req.scopeRoot = scopeCollector.CollectScopes(sourceCode, scopeParser);

            SemanticAnalyzer analyzer(nullptr);
            std::vector<Diagnostic> diagnostics;
            CHECK_NOTHROW(diagnostics = analyzer.Analyze(req));
            totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

            for (const auto &diag : diagnostics)
            {
                if (diag.code != "as-err-undeclared-identifier")
                    continue;

                ++totalFlagged;
                ++flaggedNameCounts[ExtractFlaggedName(diag.message)];
            }
        }
    }

    MESSAGE("Grouped-workspace undefined-identifier audit: groups=" << groups.size()
            << " files=" << totalFiles
            << " totalFlagged=" << totalFlagged
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));
    MESSAGE("This corpus has no .as.predefined file anywhere (confirmed by grep), so names like "
            "\"self\"/\"g_Engine\" below are unresolvable here no matter how files are grouped - "
            "that's a fact about this corpus, not a proven gap in the mechanism. The mechanism "
            "itself (Server::ParserPredefined collecting a .as.predefined-style source into the "
            "shared SymbolTable, then engine globals from it resolving in real scripts) is "
            "proven directly by \"SemanticAnalyzer - engine globals declared in a predefined-style "
            "file are not flagged\" in this same file.");

    std::vector<std::pair<std::string, size_t>> ranked(flaggedNameCounts.begin(), flaggedNameCounts.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

    MESSAGE("Top 30 most-frequently-flagged names (still-external engine symbols expected to dominate):");
    for (size_t i = 0; i < ranked.size() && i < 30; ++i)
        MESSAGE("  " << ranked[i].second << "x  " << ranked[i].first);

    CHECK(totalFiles > 0);
}

// =====================================================================================
// What counts as a local and what counts as a global.
//
// LOCALS_QUERY gives locals and module/namespace-scope globals the identical kind
// (LocalDefinitionKind::Variable), so the only thing separating them is where they were
// declared: a definition is a LOCAL when its scope, or any scope above it, was opened by
// a func_declaration or a lambda_expression (Scope::isFunctionScope). Everything else at
// Variable kind is a GLOBAL.
//
// That distinction is load-bearing in three places, which have to agree: the unused
// variable warning, the null-to-non-handle check, and the "remove unused variable" quick
// fix. Only locals are reported, because "unused" is not decidable for a global from one
// file - another file in the workspace, or the engine itself, may reference it.
// =====================================================================================

TEST_CASE("Local vs global - a variable in a function body is a local and is reported unused")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource("void Main()\n{\n    int unusedLocal = 1;\n}\n", table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "unusedLocal"));
}

TEST_CASE("Local vs global - a variable in a nested block is still a local")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "void Main()\n{\n    if (true)\n    {\n        int nested = 1;\n    }\n}\n", table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "nested"));
}

TEST_CASE("Local vs global - a variable in a method body is a local")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class Weapon\n{\n    void Fire()\n    {\n        int shots = 1;\n    }\n}\n", table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "shots"));
}

TEST_CASE("Local vs global - a variable in a lambda body is a local")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "void Main()\n{\n    auto fn = function() { int inner = 1; };\n}\n", table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "inner"));
}

TEST_CASE("Local vs global - a module-scope variable is a global and is never reported unused")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource("string WPN_NAME = \"ak47\";\n", table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "WPN_NAME"));
}

TEST_CASE("Local vs global - a namespace-scope variable is a global")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "namespace Weapons\n{\n    int roundCount = 0;\n}\n", table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "roundCount"));
}

TEST_CASE("Local vs global - a class field is neither, and is never reported unused")
{
    // A field is LocalDefinitionKind::Field, so it never reaches the Variable-only check -
    // and class_body is not a function scope either way.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource("class Weapon\n{\n    int ammo;\n}\n", table, i18n);

    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "ammo"));
}

TEST_CASE("Local vs global - an unused parameter is not reported, unlike an unused local")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "void Main(int ignored)\n{\n    int unusedLocal = 1;\n}\n", table, i18n);

    CHECK(HasUnusedVariableDiagnostic(diagnostics, "unusedLocal"));
    CHECK_FALSE(HasUnusedVariableDiagnostic(diagnostics, "ignored"));
}

TEST_CASE("Local vs global - a local shadowing a global is reported independently of it")
{
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "int count = 0;\nvoid Main()\n{\n    int count = 1;\n}\n", table, i18n);

    // Exactly one warning: the local. The global with the same name must not be swept in.
    CHECK(CountUnusedVariableDiagnostics(diagnostics, "count") == 1);
}

TEST_CASE("super - the base-constructor call is not an undeclared identifier")
{
    // The real compiler accepts this file (tests/parity/doc_p01_super_ctor.as). `super` names no
    // symbol and never will, so both the scope tree and the symbol table come up empty on it - the
    // rule has to recognise the shape instead.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class Base { Base(int x) {} }\n"
        "class Derived : Base { Derived() { super(1); } }\n",
        table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "super"));
}

TEST_CASE("super - outside a constructor it really is undeclared")
{
    // The compiler's own answer on tests/parity/doc_r01_super_method.as:
    //     ERROR (2, 40): No matching symbol 'super'
    // `super` is only the base-constructor call. It is not a handle to the base class, so
    // `super.F()` is an error and the exemption above must not reach it.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class B { void F() {} }\n"
        "class D : B { void F() { super.F(); } }\n",
        table, i18n);

    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "super"));
}

TEST_CASE("super - a class with no base cannot call one")
{
    // `super(1)` in a class that names no base is an error the compiler does report, so the
    // exemption tests for a base list rather than merely for being in a constructor.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class Lonely { Lonely() { super(1); } }\n", table, i18n);

    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "super"));
}

TEST_CASE("virtual property - a bare accessor name inside a method is not undeclared")
{
    // tests/parity/doc_p03_implicit_this_property.as compiles: with the `property` keyword, a
    // bare `Up` inside a method is this.get_Up(). The symbol is stored as get_Up, so nothing
    // named Up is ever in the table.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class C" + std::string(1, char(10)) +
        "{" + std::string(1, char(10)) +
        "    int get_Up() const property { return 1; }" + std::string(1, char(10)) +
        "    void T() { int v = Up; }" + std::string(1, char(10)) +
        "}" + std::string(1, char(10)),
        table, i18n);

    CHECK_FALSE(HasUndefinedIdentifierDiagnostic(diagnostics, "Up"));
}

TEST_CASE("virtual property - the name is only excused where an accessor declares it")
{
    // The exemption is a name set, not a blanket skip of unknown identifiers. `Down` has no
    // accessor anywhere, so it stays reported.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class C" + std::string(1, char(10)) +
        "{" + std::string(1, char(10)) +
        "    int get_Up() const property { return 1; }" + std::string(1, char(10)) +
        "    void T() { int v = Down; }" + std::string(1, char(10)) +
        "}" + std::string(1, char(10)),
        table, i18n);

    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "Down"));
}

TEST_CASE("virtual property - a get_ prefix with nothing after it is not a property")
{
    // Stripping "get_" from a member literally called `get_` leaves the empty string, and
    // inserting that would have excused every unresolved name in the workspace.
    SymbolTable table;
    angel_lsp::i18n::I18n i18n("en");
    auto diagnostics = AnalyzeSource(
        "class C" + std::string(1, char(10)) +
        "{" + std::string(1, char(10)) +
        "    int get_() const property { return 1; }" + std::string(1, char(10)) +
        "    void T() { int v = Whatever; }" + std::string(1, char(10)) +
        "}" + std::string(1, char(10)),
        table, i18n);

    CHECK(HasUndefinedIdentifierDiagnostic(diagnostics, "Whatever"));
}
