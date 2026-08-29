#include <doctest/doctest.h>

#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    /** @brief Parses sourceCode with a fresh parser/collector pair and returns its Scope tree. */
    std::unique_ptr<Scope> CollectScopesFromSource(const std::string &sourceCode)
    {
        AngelScriptParser parser;
        LocalScopeCollector collector(nullptr);
        return collector.CollectScopes(sourceCode, parser);
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

    struct SourcePos
    {
        uint32_t line;
        uint32_t character;
    };

    /** @brief Locates the 0-indexed line/character of needle's first occurrence in source, starting the search at startAt. */
    SourcePos FindPosition(const std::string &source, const std::string &needle, size_t startAt = 0)
    {
        size_t pos = source.find(needle, startAt);
        REQUIRE_MESSAGE(pos != std::string::npos, "Expected to find \"" << needle << "\" in test source");

        uint32_t line = 0;
        size_t lineStart = 0;
        for (size_t i = 0; i < pos; ++i)
        {
            if (source[i] == '\n')
            {
                ++line;
                lineStart = i + 1;
            }
        }

        return SourcePos{line, static_cast<uint32_t>(pos - lineStart)};
    }

    /** @brief Searches only scope's own references (not descendants) for one matching name. */
    const LocalReference *FindReferenceByName(const Scope *scope, const std::string &name)
    {
        for (const auto &ref : scope->references)
        {
            if (ref.name == name)
                return &ref;
        }
        return nullptr;
    }

    /** @brief Walks down from scope to the innermost child whose range contains (line, character). */
    /**
     * @brief Innermost containing scope, falling back to the scope passed in.
     *
     * Not analysis::FindInnermostScope: that reports nullptr when the point lies outside the root,
     * this returns the root. These tests hand it a scope they already know contains the point and
     * want the deepest child, so the fallback is what they mean - but the two must not share a
     * name, or a later reader will assume the null contract holds here.
     */
    const Scope *FindEnclosingScopeOrRoot(const Scope *scope, uint32_t line, uint32_t character)
    {
        for (const auto &child : scope->children)
        {
            bool afterStart = (line > child->startLine) || (line == child->startLine && character >= child->startCharacter);
            bool beforeEnd = (line < child->endLine) || (line == child->endLine && character <= child->endCharacter);

            if (afterStart && beforeEnd)
                return FindEnclosingScopeOrRoot(child.get(), line, character);
        }

        return scope;
    }

    /** @brief Recursively counts every LocalDefinition across scope and its descendants. */
    size_t CountDefinitions(const Scope *scope)
    {
        size_t count = scope->definitions.size();
        for (const auto &child : scope->children)
            count += CountDefinitions(child.get());
        return count;
    }

    /** @brief Recursively counts every Scope node (scope itself plus descendants). */
    size_t CountScopes(const Scope *scope)
    {
        size_t count = 1;
        for (const auto &child : scope->children)
            count += CountScopes(child.get());
        return count;
    }

    /** @brief Recursively counts every LocalReference across scope and its descendants. */
    size_t CountReferences(const Scope *scope)
    {
        size_t count = scope->references.size();
        for (const auto &child : scope->children)
            count += CountReferences(child.get());
        return count;
    }

    /** @brief Recursively tallies definitions by kind across scope and its descendants into counts. */
    void TallyDefinitionsByKind(const Scope *scope, std::unordered_map<std::string, size_t> &counts)
    {
        for (const auto &def : scope->definitions)
        {
            switch (def.kind)
            {
            case LocalDefinitionKind::Parameter: ++counts["Parameter"]; break;
            case LocalDefinitionKind::Variable: ++counts["Variable"]; break;
            case LocalDefinitionKind::Field: ++counts["Field"]; break;
            case LocalDefinitionKind::Function: ++counts["Function"]; break;
            case LocalDefinitionKind::Method: ++counts["Method"]; break;
            case LocalDefinitionKind::Type: ++counts["Type"]; break;
            case LocalDefinitionKind::Constant: ++counts["Constant"]; break;
            case LocalDefinitionKind::Namespace: ++counts["Namespace"]; break;
            case LocalDefinitionKind::Import: ++counts["Import"]; break;
            }
        }
        for (const auto &child : scope->children)
            TallyDefinitionsByKind(child.get(), counts);
    }
}

// =====================================================================================
// Basic definition/reference resolution
// =====================================================================================

TEST_CASE("LocalScopeCollector - parameter definition and body reference resolve together")
{
    std::string source = R"AS(
void Foo(int x)
{
    return x + 1;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "x + 1");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *resolved = ResolveInScope(innermost, "x");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "x");
    CHECK(resolved->kind == LocalDefinitionKind::Parameter);
}

// =====================================================================================
// Scope nesting and shadowing
// =====================================================================================

TEST_CASE("LocalScopeCollector - nested block shadowing resolves to the innermost definition")
{
    std::string source = R"AS(
void Foo()
{
    for (int i = 0; i < 10; i++)
    {
        if (true)
        {
            int i = 5;
            i = i + 1;
        }
    }
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "i + 1");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *resolved = ResolveInScope(innermost, "i");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->kind == LocalDefinitionKind::Variable);

    SourcePos innerDefPos = FindPosition(source, "i = 5");
    CHECK(resolved->startLine == innerDefPos.line);
    CHECK(resolved->startCharacter == innerDefPos.character);
}

TEST_CASE("LocalScopeCollector - lambda parameters are captured as definitions")
{
    // Matches the real-world shape found across 17 corpus files: an untyped lambda
    // parameter list passed as a callback argument, e.g. arr.sort(function(a, b) {...}).
    std::string source = R"AS(
void Foo()
{
    SomeCall(function(a, b) { return a < b; });
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "a < b");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *resolvedA = ResolveInScope(innermost, "a");
    REQUIRE(resolvedA != nullptr);
    CHECK(resolvedA->kind == LocalDefinitionKind::Parameter);

    const LocalDefinition *resolvedB = ResolveInScope(innermost, "b");
    REQUIRE(resolvedB != nullptr);
    CHECK(resolvedB->kind == LocalDefinitionKind::Parameter);
}

TEST_CASE("LocalScopeCollector - member access identifiers are flagged as such")
{
    std::string source = R"AS(
void Foo()
{
    obj.value = 5;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "value = 5");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalReference *memberRef = FindReferenceByName(innermost, "value");
    REQUIRE(memberRef != nullptr);
    CHECK(memberRef->isMemberAccess == true);

    // The object side of "obj.value" is an ordinary lexical reference, not a member access.
    const LocalReference *objectRef = FindReferenceByName(innermost, "obj");
    REQUIRE(objectRef != nullptr);
    CHECK(objectRef->isMemberAccess == false);
}

TEST_CASE("LocalScopeCollector - a class field is captured exactly once, as Field")
{
    // LOCALS_QUERY has both a generic (variable_declarator) pattern and a more specific
    // class_body-nested pattern for fields; without the BuildScopeTree dedup pass, a field
    // used to produce two definitions on the identical node - one correctly Field, one
    // spuriously Variable (confirmed by hand before the fix landed).
    std::string source = R"AS(
class Foo
{
    int value;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);
    REQUIRE(root->children.size() == 1);

    const Scope *classScope = root->children[0].get();
    REQUIRE(classScope->definitions.size() == 1);
    CHECK(classScope->definitions[0].name == "value");
    CHECK(classScope->definitions[0].kind == LocalDefinitionKind::Field);
}

TEST_CASE("LocalScopeCollector - a method is captured exactly once, as Method")
{
    // Same duplicate-capture shape as the field case above, for func_declaration vs the
    // class_body-nested method pattern. The method's own name belongs in class_body's own
    // definitions, not inside the method's own scope - func_declaration's range covers name +
    // parameters + body together, but a method name must be visible to callers elsewhere in
    // the class, not only recursively from within its own body (previously a known, documented
    // bug - "Bug 2" - now fixed via BuildScopeTree's own-name redirect).
    std::string source = R"AS(
class Foo
{
    void Bar() {}
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);
    REQUIRE(root->children.size() == 1);

    const Scope *classScope = root->children[0].get();
    REQUIRE(classScope->definitions.size() == 1);
    CHECK(classScope->definitions[0].name == "Bar");
    CHECK(classScope->definitions[0].kind == LocalDefinitionKind::Method);

    // The method's own scope (for its parameters/body) still exists as a child - just without
    // its own name duplicated inside it.
    REQUIRE(classScope->children.size() == 1);
    CHECK(classScope->children[0]->definitions.empty());
}

TEST_CASE("LocalScopeCollector - a function's own name resolves from outside its body, not just from within")
{
    std::string source = R"AS(
void Helper() {}

void Foo()
{
    Helper();
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    // Helper's own name lives in the root (script) scope, not inside Helper's own body.
    const LocalDefinition *helperDef = ResolveInScope(root.get(), "Helper");
    REQUIRE(helperDef != nullptr);
    CHECK(helperDef->kind == LocalDefinitionKind::Function);

    // A reference to Helper() from inside Foo's body resolves to that same definition via
    // ResolveInScope alone - no SymbolTable fallback needed.
    SourcePos refPos = FindPosition(source, "Helper();", source.find("void Foo"));
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *resolved = ResolveInScope(innermost, "Helper");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->kind == LocalDefinitionKind::Function);
    CHECK(resolved->startLine == helperDef->startLine);
    CHECK(resolved->startCharacter == helperDef->startCharacter);
}

TEST_CASE("LocalScopeCollector - method parameter shadows a class field of the same name")
{
    std::string source = R"AS(
class Foo
{
    int value;

    void SetValue(int value)
    {
        value = value;
    }
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "value = value");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *resolved = ResolveInScope(innermost, "value");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->kind == LocalDefinitionKind::Parameter);
}

// =====================================================================================
// Per-local type info (isHandleType / hasNullInitializer / typeKind / typeName), used by
// SemanticAnalyzer::CheckNullAssignedToNonHandleInScope
// =====================================================================================

TEST_CASE("LocalScopeCollector - a local variable's declared type and null initializer are captured")
{
    std::string source = R"AS(
void Foo()
{
    int f = null;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos declPos = FindPosition(source, "f = null");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), declPos.line, declPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *def = ResolveInScope(innermost, "f");
    REQUIRE(def != nullptr);
    CHECK(def->kind == LocalDefinitionKind::Variable);
    CHECK(def->isHandleType == false);
    CHECK(def->hasNullInitializer == true);
    CHECK(def->typeKind == TypeKind::Int32);
    CHECK(def->typeName == "int");
}

TEST_CASE("LocalScopeCollector - a local handle variable is not flagged as a plain null initializer")
{
    std::string source = R"AS(
void Foo()
{
    Bar@ h = null;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos declPos = FindPosition(source, "h = null");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), declPos.line, declPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *def = ResolveInScope(innermost, "h");
    REQUIRE(def != nullptr);
    CHECK(def->isHandleType == true);
    CHECK(def->hasNullInitializer == true);
}

TEST_CASE("LocalScopeCollector - null used only as a call argument is not a local null initializer")
{
    std::string source = R"AS(
void Foo()
{
    int f = someFunc(null);
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos declPos = FindPosition(source, "f = someFunc");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), declPos.line, declPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *def = ResolveInScope(innermost, "f");
    REQUIRE(def != nullptr);
    CHECK(def->hasNullInitializer == false);
}

TEST_CASE("LocalScopeCollector - a foreach loop variable has no type info populated")
{
    // foreach_variable shares LocalDefinitionKind::Variable with variable_declarator but has no
    // declared-type/initializer node at all - ReadVariableTypeInfo must leave it at defaults
    // rather than misreading an unrelated sibling node.
    std::string source = R"AS(
void Foo()
{
    array<int> items;
    foreach (int v : items)
    {
    }
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos loopPos = FindPosition(source, "int v : items");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), loopPos.line, loopPos.character);
    REQUIRE(innermost != nullptr);

    const LocalDefinition *v = ResolveInScope(innermost, "v");
    REQUIRE(v != nullptr);
    CHECK(v->hasNullInitializer == false);
    CHECK(v->isHandleType == false);
    CHECK(v->typeKind == TypeKind::Unknown);
}

// =====================================================================================
// Unresolved references
// =====================================================================================

TEST_CASE("LocalScopeCollector - reference to an undeclared name does not resolve")
{
    std::string source = R"AS(
void Foo()
{
    Undefined = 5;
}
)AS";

    auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    SourcePos refPos = FindPosition(source, "Undefined");
    const Scope *innermost = FindEnclosingScopeOrRoot(root.get(), refPos.line, refPos.character);
    REQUIRE(innermost != nullptr);

    CHECK(ResolveInScope(innermost, "Undefined") == nullptr);
}

// =====================================================================================
// Real-world script parsing tests (angelscript/ corpus)
// =====================================================================================

TEST_CASE("LocalScopeCollector - builds scope trees for real-world AngelScript files without crashing")
{
    // Same representative small/medium/large files used by the SymbolCollector smoke test,
    // for the same reason: exercise both the common case and a stress case.
    const std::vector<std::string> corpusFiles = {
        "AFBase_AFBase.as",
        "AFBase_AFBaseClass.as",
        "svencoop_ChatSounds.as",
    };

    for (const auto &fileName : corpusFiles)
    {
        std::string sourceCode = ReadCorpusFile(fileName);
        REQUIRE_MESSAGE(!sourceCode.empty(), "Expected corpus file to exist and be non-empty: " << fileName);

        std::unique_ptr<Scope> root;
        CHECK_NOTHROW(root = CollectScopesFromSource(sourceCode));
        REQUIRE_MESSAGE(root != nullptr, "Expected a root scope for: " << fileName);

        CHECK_MESSAGE(CountScopes(root.get()) > 1, "Expected nested scopes beyond the root for: " << fileName);
        CHECK_MESSAGE(CountDefinitions(root.get()) > 0, "Expected at least one local definition for: " << fileName);
    }
}

// =====================================================================================
// Full-corpus audit (opt-in - skipped by default like the SymbolCollector corpus audit in
// SymbolCollectorTest.cpp, and deliberately not registered as its own add_test() for the same
// reason documented in CMakeLists.txt: run it on demand with
// `angel_lsp_tests.exe --no-skip --test-case="*Local Scope Corpus Audit*"`)
// =====================================================================================

TEST_CASE("LocalScopeCollector - Local Scope Corpus Audit Across All angelscript Files" * doctest::skip(true))
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

    size_t totalFiles = 0;
    size_t totalScopes = 0;
    size_t totalDefinitions = 0;
    size_t totalReferences = 0;
    double totalSeconds = 0.0;
    std::unordered_map<std::string, size_t> definitionKindCounts;
    std::vector<std::string> zeroDefinitionFiles;

    for (const auto &path : files)
    {
        std::string sourceCode = ReadCorpusFile(path.filename().string());
        if (sourceCode.empty())
            continue;

        ++totalFiles;

        std::unique_ptr<Scope> root;
        auto start = std::chrono::steady_clock::now();
        CHECK_NOTHROW(root = CollectScopesFromSource(sourceCode));
        totalSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        if (!root)
            continue;

        size_t fileScopes = CountScopes(root.get());
        size_t fileDefinitions = CountDefinitions(root.get());
        size_t fileReferences = CountReferences(root.get());

        totalScopes += fileScopes;
        totalDefinitions += fileDefinitions;
        totalReferences += fileReferences;

        TallyDefinitionsByKind(root.get(), definitionKindCounts);

        if (fileDefinitions == 0)
            zeroDefinitionFiles.push_back(path.filename().string());
    }

    MESSAGE("Local scope corpus audit: files=" << totalFiles
            << " totalScopes=" << totalScopes
            << " totalDefinitions=" << totalDefinitions
            << " totalReferences=" << totalReferences
            << " totalSeconds=" << totalSeconds
            << " avgMsPerFile=" << (totalFiles ? (totalSeconds * 1000.0 / static_cast<double>(totalFiles)) : 0.0));

    for (const auto &[kindName, count] : definitionKindCounts)
        MESSAGE("  " << kindName << ": " << count);

    if (!zeroDefinitionFiles.empty())
    {
        std::string list;
        for (const auto &fileName : zeroDefinitionFiles)
        {
            list += fileName;
            list += ", ";
        }
        MESSAGE("Files that yielded zero local definitions (" << zeroDefinitionFiles.size() << "): " << list);
    }

    CHECK(totalFiles > 0);
    CHECK(totalDefinitions > 0);
}

// =====================================================================================
// Capture disambiguation. LOCALS_QUERY used to capture every variable_declarator as
// @local.definition.var without anchoring it to a context, which meant a class field
// matched both that pattern and the class_body @local.definition.field one. Anchoring
// the variable pattern to the contexts a variable_declaration can actually appear in
// outside a class body settles it in the query rather than downstream.
// =====================================================================================

namespace
{
    /** @brief Counts definitions of the given name and kind anywhere in the scope tree. */
    size_t CountDefinitions(const Scope *scope, const std::string &name, LocalDefinitionKind kind)
    {
        size_t total = 0;

        for (const auto &def : scope->definitions)
        {
            if (def.name == name && def.kind == kind)
                ++total;
        }

        for (const auto &child : scope->children)
            total += CountDefinitions(child.get(), name, kind);

        return total;
    }
}

TEST_CASE("LOCALS_QUERY - a class field is captured as a field and not also as a variable")
{
    const std::string source =
        "class Weapon\n"
        "{\n"
        "    int ammo;\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Field) == 1);
    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Variable) == 0);
}

TEST_CASE("LOCALS_QUERY - a function-body local is still captured as a variable")
{
    const std::string source =
        "void Fire()\n"
        "{\n"
        "    int ammo = 30;\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Variable) == 1);
    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Field) == 0);
}

TEST_CASE("LOCALS_QUERY - a module-scope global is still captured as a variable")
{
    const std::string source = "string WPN_NAME = \"ak47\";\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "WPN_NAME", LocalDefinitionKind::Variable) == 1);
}

TEST_CASE("LOCALS_QUERY - a namespace-scope variable is still captured")
{
    const std::string source =
        "namespace Weapons\n"
        "{\n"
        "    int shared_count = 0;\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "shared_count", LocalDefinitionKind::Variable) == 1);
}

TEST_CASE("LOCALS_QUERY - a for-loop init variable is still captured")
{
    const std::string source =
        "void Loop()\n"
        "{\n"
        "    for (int i = 0; i < 10; i++) { }\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "i", LocalDefinitionKind::Variable) == 1);
}

TEST_CASE("LOCALS_QUERY - a variable declared inside a case clause is still captured")
{
    const std::string source =
        "void Pick(int mode)\n"
        "{\n"
        "    switch (mode)\n"
        "    {\n"
        "        case 1:\n"
        "            int chosen = 5;\n"
        "            break;\n"
        "    }\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "chosen", LocalDefinitionKind::Variable) == 1);
}

TEST_CASE("LOCALS_QUERY - a field and a local sharing a name stay distinct")
{
    const std::string source =
        "class Weapon\n"
        "{\n"
        "    int ammo;\n"
        "    void Fire()\n"
        "    {\n"
        "        int ammo = 1;\n"
        "    }\n"
        "}\n";

    const auto root = CollectScopesFromSource(source);
    REQUIRE(root != nullptr);

    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Field) == 1);
    CHECK(CountDefinitions(root.get(), "ammo", LocalDefinitionKind::Variable) == 1);
}
