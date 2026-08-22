#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <tree_sitter/api.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /** @brief Parses sourceCode with a fresh parser/collector pair into table. SymbolTable holds a
     *         mutex and is non-copyable, so the caller-owned table is filled in place rather than returned. */
    void CollectFromSource(const std::string &sourceCode, SymbolTable &table, const std::string &fileUri = "file:///test.as")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        collector.CollectSymbols(fileUri, sourceCode, parser, table);
    }

    /** @brief Reads an entire file from the angelscript/ corpus into memory; empty string if missing. */
    std::string ReadCorpusFile(const std::string &fileName)
    {
        std::string path = std::string(ANGELSCRIPT_CORPUS_DIR) + "/" + fileName;
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return "";

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
}

// =====================================================================================
// Out-of-body call reference tests
// =====================================================================================

TEST_CASE("SymbolCollector - Global Function Call In Variable Initializer Is Captured As Reference")
{
    // "int g = Foo();" - Foo() is called outside of any function body.
    SymbolTable table;
    CollectFromSource("int g = Foo();", table);

    auto refs = table.FindSymbols("Foo");
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].type == SymbolType::CallReference);
    CHECK(refs[0].containerName.empty());

    const auto &callSig = refs[0].GetCallReference();
    CHECK(callSig.calleeName == "Foo");
    CHECK(callSig.isMethodCall == false);
}

TEST_CASE("SymbolCollector - Namespace-Qualified Call In Initializer Keeps Full Scoped Name")
{
    // "int g = NS::Foo();" - the grammar always parses this call target as a
    // scoped_identifier (never a bare identifier), even for unqualified calls.
    SymbolTable table;
    CollectFromSource("int g = NS::Foo();", table);

    auto refs = table.FindSymbols("NS::Foo");
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].type == SymbolType::CallReference);
    CHECK(refs[0].GetCallReference().calleeName == "NS::Foo");
    CHECK(refs[0].GetCallReference().isMethodCall == false);
}

TEST_CASE("SymbolCollector - Class Field Initializer Call Is Captured With Class As Container")
{
    std::string sourceCode = "class MyClass\n{\n    int x = ComputeDefault();\n}\n";
    SymbolTable table;
    CollectFromSource(sourceCode, table);

    auto refs = table.FindSymbols("MyClass::ComputeDefault");
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].type == SymbolType::CallReference);
    CHECK(refs[0].containerName == "MyClass");
    CHECK(refs[0].GetCallReference().calleeName == "ComputeDefault");
}

TEST_CASE("SymbolCollector - Method Call Body Location Determines Reference Capture")
{
    SUBCASE("Method call outside a function body is captured")
    {
        std::string sourceCode = "SomeClass g_obj;\nint g_value = g_obj.GetValue();\n";
        SymbolTable table;
        CollectFromSource(sourceCode, table);

        auto refs = table.FindSymbols("GetValue");
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].type == SymbolType::CallReference);

        const auto &callSig = refs[0].GetCallReference();
        CHECK(callSig.calleeName == "GetValue");
        CHECK(callSig.isMethodCall == true);
        CHECK(callSig.objectExpression == "g_obj");
    }

    SUBCASE("Method call inside a function body is not captured as a reference")
    {
        std::string sourceCode = "void Main()\n{\n    SomeClass obj;\n    obj.GetValue();\n}\n";
        SymbolTable table;
        CollectFromSource(sourceCode, table);

        // "Main" itself is still collected as a function...
        CHECK(table.HasSymbol("Main") == true);
        // ...but the in-body call is intentionally not, since it belongs to
        // that function's own body analysis instead of the out-of-body reference table.
        CHECK(table.FindSymbols("GetValue").empty() == true);
    }
}

TEST_CASE("SymbolCollector - Global Function Call Inside A Function Body Is Not Captured")
{
    std::string sourceCode = "void DoStuff() {}\nvoid Main()\n{\n    DoStuff();\n}\n";
    SymbolTable table;
    CollectFromSource(sourceCode, table);

    // "DoStuff" is collected once, as its own function definition - the call
    // to it from inside Main() must not add a second (reference) entry.
    auto doStuffSymbols = table.FindSymbols("DoStuff");
    REQUIRE(doStuffSymbols.size() == 1);
    CHECK(doStuffSymbols[0].type == SymbolType::Function);
}

TEST_CASE("SymbolCollector - FullRange And SelectionRange Captured Accurately")
{
    std::string sourceCode = "class MyClass\n{\n    void DoWork(int a) {}\n}\n";
    SymbolTable table;
    CollectFromSource(sourceCode, table);

    auto classSyms = table.FindSymbols("MyClass");
    REQUIRE(classSyms.size() == 1);
    // Full class extends from line 0 to line 3
    CHECK(classSyms[0].fullRange.startLine == 0);
    CHECK(classSyms[0].fullRange.endLine == 3);
    // Selection range is just the identifier "MyClass" on line 0
    CHECK(classSyms[0].selectionRange.startLine == 0);
    CHECK(classSyms[0].selectionRange.startCharacter == 6);
    CHECK(classSyms[0].selectionRange.endCharacter == 13);

    auto methodSyms = table.FindSymbols("MyClass::DoWork");
    REQUIRE(methodSyms.size() == 1);
    CHECK(methodSyms[0].fullRange.startLine == 2);
    CHECK(methodSyms[0].selectionRange.startLine == 2);
    CHECK(methodSyms[0].selectionRange.startCharacter == 9);
    CHECK(methodSyms[0].selectionRange.endCharacter == 15);
}

// =====================================================================================

// hasNullInitializer detection (IsNullInitializer must look only at the initializer's
// own top-level node, not recurse into call arguments / operands)
// =====================================================================================

TEST_CASE("SymbolCollector - Direct Null Initializer Is Detected")
{
    SymbolTable table;
    CollectFromSource("int f = null;", table);

    auto symbols = table.FindSymbols("f");
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].GetVariable().hasNullInitializer == true);
}

TEST_CASE("SymbolCollector - Parenthesized Null Initializer Is Detected")
{
    SymbolTable table;
    CollectFromSource("int f = (null);", table);

    auto symbols = table.FindSymbols("f");
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].GetVariable().hasNullInitializer == true);
}

TEST_CASE("SymbolCollector - Handle Variable Null Initializer Is Detected")
{
    SymbolTable table;
    CollectFromSource("Foo@ h = null;", table);

    auto symbols = table.FindSymbols("h");
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].GetVariable().hasNullInitializer == true);
    CHECK(symbols[0].GetVariable().modifiers.isHandle == true);
}

TEST_CASE("SymbolCollector - Null Used As Call Argument Is Not A Null Initializer")
{
    // "int f = someFunc(null);" - null appears in the tree, but the value actually
    // assigned to f is someFunc's return value, not null itself.
    SymbolTable table;
    CollectFromSource("int f = someFunc(null);", table);

    auto symbols = table.FindSymbols("f");
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].GetVariable().hasNullInitializer == false);
}

TEST_CASE("SymbolCollector - Non Null Initializer Is Not Flagged")
{
    SymbolTable table;
    CollectFromSource("int f = 5;", table);

    auto symbols = table.FindSymbols("f");
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].GetVariable().hasNullInitializer == false);
}

// =====================================================================================
// Baseline declaration collection sanity checks (guard against refactor regressions)
// =====================================================================================

TEST_CASE("SymbolCollector - Global Variable Test")
{
    SymbolTable table;
    CollectFromSource("int property;", table);

    CHECK(table.HasSymbol("property") == true);
    auto symbols = table.FindSymbols("property");
    REQUIRE(symbols.size() == 1);

    const auto &sym = symbols[0];
    CHECK(sym.type == SymbolType::Variable);
    CHECK(sym.GetVariable().typeName == "int");
}

TEST_CASE("SymbolCollector - Numeric TypeKind Values Do Not Collide")
{
    // int64/uint8 and uint64/float previously shared underlying enum values because
    // the "Int = Int32" / "UInt = UInt32" aliases in TypeKind reset the implicit
    // enumerator counter mid-sequence. Guard that int64, uint8, uint64, and float
    // each classify to their own distinct TypeKind.
    SymbolTable table;
    CollectFromSource(
        "int64 a;\n"
        "uint8 b;\n"
        "uint64 c;\n"
        "float d;\n",
        table);

    auto getKind = [&](const std::string &name)
    {
        auto symbols = table.FindSymbols(name);
        REQUIRE(symbols.size() == 1);
        return symbols[0].GetVariable().typeKind;
    };

    TypeKind aKind = getKind("a");
    TypeKind bKind = getKind("b");
    TypeKind cKind = getKind("c");
    TypeKind dKind = getKind("d");

    CHECK(aKind == TypeKind::Int64);
    CHECK(bKind == TypeKind::UInt8);
    CHECK(cKind == TypeKind::UInt64);
    CHECK(dKind == TypeKind::Float);

    CHECK(aKind != bKind);
    CHECK(cKind != dKind);
}

TEST_CASE("SymbolCollector - Function Declaration Test")
{
    SymbolTable table;
    CollectFromSource("void main()\n{\n    int i = 0;\n}\n", table);

    CHECK(table.HasSymbol("main") == true);
    auto mainSymbols = table.FindSymbols("main");
    REQUIRE(mainSymbols.size() == 1);
    CHECK(mainSymbols[0].type == SymbolType::Function);
    CHECK(mainSymbols[0].GetFunction().returnType == "void");
}

TEST_CASE("SymbolCollector - Class Inheritance Extraction Test")
{
    std::string sourceCode = "class ClassHereny : ClassBase\n{\n    void MyMethod() {}\n}\n";
    SymbolTable table;
    CollectFromSource(sourceCode, table);

    auto classSymbols = table.FindSymbols("ClassHereny");
    REQUIRE(classSymbols.size() == 1);
    REQUIRE(classSymbols[0].GetClass().bases.size() == 1);
    CHECK(classSymbols[0].GetClass().bases[0] == "ClassBase");
}

// =====================================================================================
// Validation diagnostics (query-driven CheckUsingDeclarationCapture / CheckDuplicateModifierGroup)
// =====================================================================================

TEST_CASE("SymbolCollector - Using Namespace Reserved Keyword")
{
    std::string sourceCode = "using namespace class;\nusing namespace int;\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-reserved-keyword-name");
    CHECK(diagnostics[1].code == "as-err-reserved-keyword-name");
}

TEST_CASE("SymbolCollector - Using Namespace Valid Identifier No Diagnostic")
{
    std::string sourceCode = "using namespace Foo;\nusing namespace Foo::Bar;\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    CHECK(diagnostics.empty() == true);
}

TEST_CASE("SymbolCollector - Duplicate Declaration Modifier Warning On Class, Mixin, And Interface")
{
    // The interface case exercises shared_external_modifier (not declaration_modifier,
    // which class/mixin use) - the query-driven check handles both node kinds, whereas
    // the previous manual walk only ever checked for declaration_modifier and so never
    // flagged duplicate modifiers on interfaces at all.
    std::string sourceCode =
        "final final class Foo {}\n"
        "shared shared interface Bar {}\n"
        "mixin external external class Baz {}\n";
    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols("file:///test.as", sourceCode, parser, table);

    REQUIRE(diagnostics.size() == 3);
    for (const auto &diag : diagnostics)
    {
        CHECK(diag.code == "as-err-attribute-repeated");
        CHECK(diag.severity == DiagnosticSeverity::Warning);
    }
}

// =====================================================================================
// Real-world script parsing tests (angelscript/ corpus)
// =====================================================================================

TEST_CASE("SymbolCollector - Parses Real-World AngelScript Files Without Crashing")
{
    // A small file, a medium one, and a large (~190KB) one, to exercise both the
    // common case and a stress case for the TAGS_QUERY dispatch and parse-error reporting.
    const std::vector<std::string> corpusFiles = {
        "AFBase_AFBase.as",
        "AFBase_AFBaseClass.as",
        "svencoop_ChatSounds.as",
    };

    for (const auto &fileName : corpusFiles)
    {
        std::string sourceCode = ReadCorpusFile(fileName);
        REQUIRE_MESSAGE(!sourceCode.empty(), "Expected corpus file to exist and be non-empty: " << fileName);

        SymbolTable table;
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);

        std::vector<Diagnostic> diagnostics;
        CHECK_NOTHROW(diagnostics = collector.CollectSymbols("file:///" + fileName, sourceCode, parser, table));

        // A real-world script of non-trivial size should yield at least one collected symbol.
        bool hasAnySymbol = false;
        table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
        {
            if (!symbols.empty())
                hasAnySymbol = true;
        });
        CHECK_MESSAGE(hasAnySymbol == true, "Expected at least one symbol collected from: " << fileName);
    }
}

// =====================================================================================
// Full-corpus audit (opt-in - skipped by default, run via `ctest -R angel_lsp_corpus_audit`
// or `angel_lsp_tests.exe --no-skip --test-case="*Corpus Audit*"`)
// =====================================================================================

TEST_CASE("SymbolCollector - Corpus Audit Across All angelscript Files" * doctest::skip(true))
{
    namespace fs = std::filesystem;

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(ANGELSCRIPT_CORPUS_DIR))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".as")
            files.push_back(entry.path());
    }
    REQUIRE_MESSAGE(!files.empty(), "Expected the angelscript/ corpus directory to contain .as files");
    std::sort(files.begin(), files.end());

    // Cheap, independent heuristics for gross under-collection - not exact (comments/strings
    // can skew a text-based scan), just a smoke signal alongside the zero-symbol check below.
    static const std::regex classRegex(R"(\bclass\s+[A-Za-z_]\w*)");
    static const std::regex funcRegex(R"(\b(?:void|bool|int|int8|int16|int32|int64|uint|uint8|uint16|uint32|uint64|float|double|string)\s+[A-Za-z_]\w*\s*\()");

    size_t totalFiles = 0;
    size_t totalSymbols = 0;
    size_t totalParseErrorDiagnostics = 0;
    size_t totalRegexClassHits = 0;
    size_t totalRegexFuncHits = 0;
    size_t totalCollectedClasses = 0;
    size_t totalCollectedFunctions = 0;
    double totalSeconds = 0.0;
    std::unordered_map<std::string, size_t> symbolTypeCounts;
    std::vector<std::string> zeroSymbolCleanParseFiles;

    for (const auto &path : files)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        std::string sourceCode = buffer.str();
        if (sourceCode.empty())
            continue;

        ++totalFiles;

        SymbolTable table;
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);

        std::vector<Diagnostic> diagnostics;
        auto start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(diagnostics = collector.CollectSymbols("file:///" + path.filename().string(), sourceCode, parser, table));
        totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        size_t parseErrors = 0;
        for (const auto &diag : diagnostics)
        {
            if (diag.code == "as-syntax-error")
                ++parseErrors;
        }
        totalParseErrorDiagnostics += parseErrors;

        size_t fileSymbolCount = 0;
        table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
        {
            fileSymbolCount += symbols.size();
            for (const auto &sym : symbols)
            {
                ++symbolTypeCounts[SymbolTypeToString(sym.type)];
                if (sym.type == SymbolType::Class)
                    ++totalCollectedClasses;
                else if (sym.type == SymbolType::Function)
                    ++totalCollectedFunctions;
            }
        });
        totalSymbols += fileSymbolCount;

        if (parseErrors == 0 && fileSymbolCount == 0)
            zeroSymbolCleanParseFiles.push_back(path.filename().string());

        totalRegexClassHits += static_cast<size_t>(std::distance(
            std::sregex_iterator(sourceCode.begin(), sourceCode.end(), classRegex), std::sregex_iterator()));
        totalRegexFuncHits += static_cast<size_t>(std::distance(
            std::sregex_iterator(sourceCode.begin(), sourceCode.end(), funcRegex), std::sregex_iterator()));
    }

    MESSAGE("Corpus audit: files=" << totalFiles
            << " totalSymbols=" << totalSymbols
            << " totalParseErrorDiagnostics=" << totalParseErrorDiagnostics
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));

    for (const auto &[typeName, count] : symbolTypeCounts)
        MESSAGE("  " << typeName << ": " << count);

    MESSAGE("Regex sanity check: class keyword occurrences=" << totalRegexClassHits
            << " vs collected Class symbols=" << totalCollectedClasses
            << " | function-shaped occurrences=" << totalRegexFuncHits
            << " vs collected Function symbols=" << totalCollectedFunctions);

    if (!zeroSymbolCleanParseFiles.empty())
    {
        std::string list;
        for (const auto &fileName : zeroSymbolCleanParseFiles)
        {
            list += fileName;
            list += ", ";
        }
        MESSAGE("Files that parsed with zero syntax errors but yielded zero symbols ("
                << zeroSymbolCleanParseFiles.size() << "): " << list);
    }

    CHECK(totalFiles > 0);
    CHECK(totalSymbols > 0);
}
