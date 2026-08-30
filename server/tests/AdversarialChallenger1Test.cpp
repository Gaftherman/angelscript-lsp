#include <doctest/doctest.h>

#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "utils/IncludeResolver.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::utils;

namespace
{
    /**
     * @brief RAII helper for creating and cleaning temporary directories for disk tests.
     */
    struct TempDirGuard
    {
        std::filesystem::path dir;

        explicit TempDirGuard(const std::string &prefix)
        {
            auto base = std::filesystem::temp_directory_path();
            auto uniqueSuffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            dir = base / (prefix + "_" + uniqueSuffix);
            std::filesystem::create_directories(dir);
        }

        ~TempDirGuard()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        void WriteFile(const std::string &relativePath, const std::string &content)
        {
            std::filesystem::path fullPath = dir / relativePath;
            if (fullPath.has_parent_path())
            {
                std::filesystem::create_directories(fullPath.parent_path());
            }
            std::ofstream out(fullPath, std::ios::binary);
            out << content;
            out.close();
        }

        std::string PathString(const std::string &relativePath = "") const
        {
            std::filesystem::path p = relativePath.empty() ? dir : (dir / relativePath);
            std::error_code ec;
            std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
            std::string s = canon.string();
#if defined(_WIN32)
            if (s.rfind("\\\\?\\", 0) == 0)
            {
                s = s.substr(4);
            }
#endif
            std::replace(s.begin(), s.end(), '\\', '/');
            return s;
        }
    };

    /**
     * @brief Helper to validate range containment recursively for DocumentSymbol hierarchy.
     */
    void AssertRangeHierarchy(const lsp::DocumentSymbol &sym)
    {
        // 1. Range start <= selectionRange start
        if (sym.range.start.line == sym.selectionRange.start.line)
        {
            CHECK(sym.range.start.character <= sym.selectionRange.start.character);
        }
        else
        {
            CHECK(sym.range.start.line <= sym.selectionRange.start.line);
        }

        // 2. SelectionRange end <= Range end
        if (sym.selectionRange.end.line == sym.range.end.line)
        {
            CHECK(sym.selectionRange.end.character <= sym.range.end.character);
        }
        else
        {
            CHECK(sym.selectionRange.end.line <= sym.range.end.line);
        }

        // 3. SelectionRange start <= SelectionRange end
        if (sym.selectionRange.start.line == sym.selectionRange.end.line)
        {
            CHECK(sym.selectionRange.start.character <= sym.selectionRange.end.character);
        }
        else
        {
            CHECK(sym.selectionRange.start.line <= sym.selectionRange.end.line);
        }

        if (sym.children.has_value())
        {
            for (const auto &child : sym.children.value())
            {
                // Child range must be within parent range
                if (sym.range.start.line == child.range.start.line)
                {
                    CHECK(sym.range.start.character <= child.range.start.character);
                }
                else
                {
                    CHECK(sym.range.start.line <= child.range.start.line);
                }

                if (child.range.end.line == sym.range.end.line)
                {
                    CHECK(child.range.end.character <= sym.range.end.character);
                }
                else
                {
                    CHECK(child.range.end.line <= sym.range.end.line);
                }

                AssertRangeHierarchy(child);
            }
        }
    }
}

// =====================================================================================
// SECTION 1: ADVERSARIAL DOCUMENT SYMBOLS TESTS
// =====================================================================================

TEST_CASE("Adversarial DocumentSymbols - Deeply Nested Multi-Tier Namespaces")
{
    std::string code =
        "namespace Tier1\n"
        "{\n"
        "    namespace Tier2\n"
        "    {\n"
        "        namespace Tier3\n"
        "        {\n"
        "            namespace Tier4\n"
        "            {\n"
        "                class DeepWorker\n"
        "                {\n"
        "                    int m_val;\n"
        "                    DeepWorker() {}\n"
        "                    ~DeepWorker() {}\n"
        "                    void Execute(int param1, const string &in param2) {}\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///deep_namespace.as";
    DocumentSymbolRequest req{ uri, code, nullptr, table };
    auto result = GetDocumentSymbols(req);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);

    const auto &tier1 = (*result)[0];
    CHECK(tier1.name == "Tier1");
    CHECK(tier1.kind == lsp::SymbolKind::Namespace);
    REQUIRE(tier1.children.has_value());
    REQUIRE(tier1.children->size() == 1);

    const auto &tier2 = (*tier1.children)[0];
    CHECK(tier2.name == "Tier2");
    CHECK(tier2.kind == lsp::SymbolKind::Namespace);
    REQUIRE(tier2.children.has_value());
    REQUIRE(tier2.children->size() == 1);

    const auto &tier3 = (*tier2.children)[0];
    CHECK(tier3.name == "Tier3");
    CHECK(tier3.kind == lsp::SymbolKind::Namespace);
    REQUIRE(tier3.children.has_value());
    REQUIRE(tier3.children->size() == 1);

    const auto &tier4 = (*tier3.children)[0];
    CHECK(tier4.name == "Tier4");
    CHECK(tier4.kind == lsp::SymbolKind::Namespace);
    REQUIRE(tier4.children.has_value());
    REQUIRE(tier4.children->size() == 1);

    const auto &deepWorker = (*tier4.children)[0];
    CHECK(deepWorker.name == "DeepWorker");
    CHECK(deepWorker.kind == lsp::SymbolKind::Class);
    REQUIRE(deepWorker.children.has_value());
    REQUIRE(deepWorker.children->size() == 4);

    CHECK((*deepWorker.children)[0].name == "m_val");
    CHECK((*deepWorker.children)[0].kind == lsp::SymbolKind::Field);

    CHECK((*deepWorker.children)[1].name == "DeepWorker");
    CHECK((*deepWorker.children)[1].kind == lsp::SymbolKind::Constructor);

    CHECK((*deepWorker.children)[2].name == "~DeepWorker");
    CHECK((*deepWorker.children)[2].kind == lsp::SymbolKind::Constructor);

    CHECK((*deepWorker.children)[3].name == "Execute");
    CHECK((*deepWorker.children)[3].kind == lsp::SymbolKind::Method);

    AssertRangeHierarchy(tier1);
}

TEST_CASE("Adversarial DocumentSymbols - Sibling Namespaces and Multi-Class Containers")
{
    std::string code =
        "namespace Alpha\n"
        "{\n"
        "    class ClassA {}\n"
        "    class ClassB {}\n"
        "}\n"
        "namespace Beta\n"
        "{\n"
        "    class ClassC {}\n"
        "    interface InterfaceD {}\n"
        "}\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///siblings.as";
    DocumentSymbolRequest req{ uri, code, nullptr, table };
    auto result = GetDocumentSymbols(req);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);

    const auto &alpha = (*result)[0];
    CHECK(alpha.name == "Alpha");
    CHECK(alpha.kind == lsp::SymbolKind::Namespace);
    REQUIRE(alpha.children.has_value());
    REQUIRE(alpha.children->size() == 2);
    CHECK((*alpha.children)[0].name == "ClassA");
    CHECK((*alpha.children)[0].kind == lsp::SymbolKind::Class);
    CHECK((*alpha.children)[1].name == "ClassB");
    CHECK((*alpha.children)[1].kind == lsp::SymbolKind::Class);

    const auto &beta = (*result)[1];
    CHECK(beta.name == "Beta");
    CHECK(beta.kind == lsp::SymbolKind::Namespace);
    REQUIRE(beta.children.has_value());
    REQUIRE(beta.children->size() == 2);
    CHECK((*beta.children)[0].name == "ClassC");
    CHECK((*beta.children)[0].kind == lsp::SymbolKind::Class);
    CHECK((*beta.children)[1].name == "InterfaceD");
    CHECK((*beta.children)[1].kind == lsp::SymbolKind::Interface);

    for (const auto &sym : *result)
    {
        AssertRangeHierarchy(sym);
    }
}

TEST_CASE("Adversarial DocumentSymbols - Mixin Class Support")
{
    std::string code =
        "mixin class SerializableMixin\n"
        "{\n"
        "    int version;\n"
        "    void Serialize() {}\n"
        "}\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///mixin.as";
    DocumentSymbolRequest req{ uri, code, nullptr, table };
    auto result = GetDocumentSymbols(req);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);

    const auto &mixinSym = (*result)[0];
    CHECK(mixinSym.name == "SerializableMixin");
    CHECK(mixinSym.kind == lsp::SymbolKind::Class);
    CHECK(mixinSym.detail.has_value());
    CHECK(mixinSym.detail.value() == "mixin");
    REQUIRE(mixinSym.children.has_value());
    REQUIRE(mixinSym.children->size() == 2);
    CHECK((*mixinSym.children)[0].name == "version");
    CHECK((*mixinSym.children)[0].kind == lsp::SymbolKind::Field);
    CHECK((*mixinSym.children)[1].name == "Serialize");
    CHECK((*mixinSym.children)[1].kind == lsp::SymbolKind::Method);
}

TEST_CASE("Adversarial DocumentSymbols - Enums with Explicit Values (Hex, Negatives, Expressions)")
{
    std::string code =
        "enum SpecialFlags\n"
        "{\n"
        "    FLAG_NONE = 0,\n"
        "    FLAG_READ = 0x01,\n"
        "    FLAG_WRITE = 0x02,\n"
        "    FLAG_ALL = FLAG_READ | FLAG_WRITE,\n"
        "    FLAG_INVALID = -1\n"
        "}\n"
        "enum EmptyEnum {}\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///enums.as";
    DocumentSymbolRequest req{ uri, code, nullptr, table };
    auto result = GetDocumentSymbols(req);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);

    const auto &flags = (*result)[0];
    CHECK(flags.name == "SpecialFlags");
    CHECK(flags.kind == lsp::SymbolKind::Enum);
    REQUIRE(flags.children.has_value());
    REQUIRE(flags.children->size() == 5);

    CHECK((*flags.children)[0].name == "FLAG_NONE");
    CHECK((*flags.children)[0].detail == "= 0");

    CHECK((*flags.children)[1].name == "FLAG_READ");
    CHECK((*flags.children)[1].detail == "= 0x01");

    CHECK((*flags.children)[2].name == "FLAG_WRITE");
    CHECK((*flags.children)[2].detail == "= 0x02");

    CHECK((*flags.children)[3].name == "FLAG_ALL");
    CHECK((*flags.children)[3].detail == "= FLAG_READ | FLAG_WRITE");

    CHECK((*flags.children)[4].name == "FLAG_INVALID");
    CHECK((*flags.children)[4].detail == "= -1");

    const auto &emptyEnum = (*result)[1];
    CHECK(emptyEnum.name == "EmptyEnum");
    CHECK(emptyEnum.kind == lsp::SymbolKind::Enum);
    CHECK((!emptyEnum.children.has_value() || emptyEnum.children->empty()));
}

TEST_CASE("Adversarial DocumentSymbols - Complex Global Variables and Multi-Declarators")
{
    std::string code =
        "const int g_ConstA = 10, g_ConstB = 20;\n"
        "array<string>@ g_HandleArray;\n"
        "dictionary@ g_Dict = null;\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///globals.as";
    DocumentSymbolRequest req{ uri, code, nullptr, table };
    auto result = GetDocumentSymbols(req);

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 4);

    CHECK((*result)[0].name == "g_ConstA");
    CHECK((*result)[0].kind == lsp::SymbolKind::Variable);
    CHECK((*result)[0].detail == "const int");

    CHECK((*result)[1].name == "g_ConstB");
    CHECK((*result)[1].kind == lsp::SymbolKind::Variable);
    CHECK((*result)[1].detail == "const int");

    CHECK((*result)[2].name == "g_HandleArray");
    CHECK((*result)[2].kind == lsp::SymbolKind::Variable);
    CHECK((*result)[2].detail == "array<string>@");

    CHECK((*result)[3].name == "g_Dict");
    CHECK((*result)[3].kind == lsp::SymbolKind::Variable);
    CHECK((*result)[3].detail == "dictionary@");

    for (const auto &sym : *result)
    {
        AssertRangeHierarchy(sym);
    }
}

TEST_CASE("Adversarial DocumentSymbols - Empty and Whitespace Only Files")
{
    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///empty.as";

    // 1. Completely empty
    {
        DocumentSymbolRequest req{ uri, "", nullptr, table };
        auto result = GetDocumentSymbols(req);
        REQUIRE(result.has_value());
        CHECK(result->empty());
    }

    // 2. Whitespaces, tabs, carriage returns
    {
        std::string ws = "   \t\t\r\n\r\n   \t\n";
        DocumentSymbolRequest req{ uri, ws, nullptr, table };
        auto result = GetDocumentSymbols(req);
        REQUIRE(result.has_value());
        CHECK(result->empty());
    }
}

TEST_CASE("Adversarial DocumentSymbols - Malformed and Incomplete Code Recovery")
{
    // Check that malformed AST does not crash and recovers valid symbols
    std::string malformedCode =
        "class ValidClassBefore\n"
        "{\n"
        "    int a;\n"
        "}\n"
        "class IncompleteClass {\n"
        "    void (\n"
        "    int broken = ;\n"
        "namespace {\n"
        "void (@@@) {}\n"
        "class ValidClassAfter\n"
        "{\n"
        "    void DoWork() {}\n"
        "}\n";

    AngelScriptParser parser;
    SymbolTable table;
    std::string uri = "file:///malformed.as";
    DocumentSymbolRequest req{ uri, malformedCode, nullptr, table };

    CHECK_NOTHROW({
        auto result = GetDocumentSymbols(req);
        REQUIRE(result.has_value());
        bool foundValidBefore = false;
        bool foundValidAfter = false;
        for (const auto &sym : *result)
        {
            if (sym.name == "ValidClassBefore") foundValidBefore = true;
            if (sym.name == "ValidClassAfter") foundValidAfter = true;
        }
        CHECK(foundValidBefore);
        CHECK(foundValidAfter);
    });
}

// =====================================================================================
// SECTION 2: ADVERSARIAL WORKSPACE SYMBOLS TESTS
// =====================================================================================

TEST_CASE("Adversarial WorkspaceSymbols - Exact, Prefix, Substring, and Fuzzy Score Hierarchy")
{
    WorkspaceSymbolRequest req{ "", SymbolTable{}, 100 };
    SymbolCollector collector{ nullptr };
    AngelScriptParser parser;
    SymbolTable table;

    std::string code =
        "class StateManager {};\n"
        "class State {};\n"
        "class GameStateManager {};\n"
        "class AppStateController {};\n"
        "void RunState() {}\n";

    collector.CollectSymbols("file:///state_test.as", code, parser, table);

    // 1. Exact match query "State" -> "State" should rank #1
    {
        WorkspaceSymbolRequest exactReq{ "State", table, 100 };
        auto res = GetWorkspaceSymbols(exactReq);
        REQUIRE(res.has_value());
        REQUIRE(!res->empty());
        CHECK((*res)[0].name == "State");
    }

    // 2. Case difference: "state" matches "State" (exact score 100) vs "StateManager" (prefix score 80)
    {
        WorkspaceSymbolRequest caseReq{ "state", table, 100 };
        auto res = GetWorkspaceSymbols(caseReq);
        REQUIRE(res.has_value());
        REQUIRE(!res->empty());
        CHECK((*res)[0].name == "State");
    }

    // 3. Subsequence fuzzy match "gsm" -> "GameStateManager"
    {
        WorkspaceSymbolRequest fuzzyReq{ "gsm", table, 100 };
        auto res = GetWorkspaceSymbols(fuzzyReq);
        REQUIRE(res.has_value());
        REQUIRE(!res->empty());
        CHECK((*res)[0].name == "GameStateManager");
    }

    // 4. Subsequence fuzzy match "sm" -> "StateManager", "GameStateManager"
    {
        WorkspaceSymbolRequest fuzzyReq2{ "sm", table, 100 };
        auto res = GetWorkspaceSymbols(fuzzyReq2);
        REQUIRE(res.has_value());
        CHECK(res->size() >= 2);
    }
}

TEST_CASE("Adversarial WorkspaceSymbols - Qualified Names Matching")
{
    SymbolCollector collector{ nullptr };
    AngelScriptParser parser;
    SymbolTable table;

    std::string code =
        "namespace Physics\n"
        "{\n"
        "    namespace RigidBody\n"
        "    {\n"
        "        class Collider\n"
        "        {\n"
        "            void CheckCollision() {}\n"
        "        }\n"
        "    }\n"
        "}\n";

    collector.CollectSymbols("file:///physics.as", code, parser, table);

    // 1. Full namespace qualifier "Physics::RigidBody::Collider"
    {
        WorkspaceSymbolRequest req{ "Physics::RigidBody", table, 100 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        REQUIRE(!res->empty());
        bool foundCollider = false;
        for (const auto &sym : *res)
        {
            if (sym.name == "Collider") foundCollider = true;
        }
        CHECK(foundCollider);
    }

    // 2. Sub-scope qualifier "RigidBody::Collider"
    {
        WorkspaceSymbolRequest req{ "RigidBody::Collider", table, 100 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        REQUIRE(!res->empty());
        bool foundCollider = false;
        for (const auto &sym : *res)
        {
            if (sym.name == "Collider") foundCollider = true;
        }
        CHECK(foundCollider);
    }
}

TEST_CASE("Adversarial WorkspaceSymbols - Large Result Limits and MaxResults Truncation")
{
    SymbolCollector collector{ nullptr };
    AngelScriptParser parser;
    SymbolTable table;

    std::string code;
    for (int i = 0; i < 200; ++i)
    {
        code += "int symbol_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }

    collector.CollectSymbols("file:///many_symbols.as", code, parser, table);

    // 1. maxResults = 0 -> empty result
    {
        WorkspaceSymbolRequest req{ "symbol_", table, 0 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        CHECK(res->empty());
    }

    // 2. maxResults = 1 -> exactly 1 result
    {
        WorkspaceSymbolRequest req{ "symbol_", table, 1 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        CHECK(res->size() == 1);
    }

    // 3. maxResults = 50 -> exactly 50 results
    {
        WorkspaceSymbolRequest req{ "symbol_", table, 50 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        CHECK(res->size() == 50);
    }

    // 4. maxResults = 50000 -> all 200 results returned without crash
    {
        WorkspaceSymbolRequest req{ "symbol_", table, 50000 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        CHECK(res->size() == 200);
    }

    // 5. Empty query with large maxResults
    {
        WorkspaceSymbolRequest req{ "", table, 50000 };
        auto res = GetWorkspaceSymbols(req);
        REQUIRE(res.has_value());
        CHECK(res->size() == 200);
    }
}

TEST_CASE("Adversarial WorkspaceSymbols - All SymbolKind and Container Mappings")
{
    SymbolCollector collector{ nullptr };
    AngelScriptParser parser;
    SymbolTable table;

    std::string code =
        "namespace GlobalNS {\n"
        "    class MyClass {\n"
        "        int myField;\n"
        "        void myMethod() {}\n"
        "    }\n"
        "    interface MyInterface {\n"
        "        void myInterfaceMethod();\n"
        "    }\n"
        "    enum MyEnum { EnumVal1 }\n"
        "    typedef uint MyTypedef;\n"
        "    funcdef void MyFuncdef();\n"
        "}\n"
        "int myGlobalVar = 42;\n"
        "void myGlobalFunc() {}\n";

    collector.CollectSymbols("file:///all_kinds.as", code, parser, table);

    // Check mapping of each symbol
    auto res = GetWorkspaceSymbols(WorkspaceSymbolRequest{ "", table, 100 });
    REQUIRE(res.has_value());

    std::unordered_map<std::string, lsp::SymbolInformation> symMap;
    for (const auto &info : *res)
    {
        symMap[info.name] = info;
    }

    CHECK(symMap.count("GlobalNS") > 0);
    CHECK(symMap["GlobalNS"].kind == lsp::SymbolKind::Namespace);

    CHECK(symMap.count("MyClass") > 0);
    CHECK(symMap["MyClass"].kind == lsp::SymbolKind::Class);

    CHECK(symMap.count("MyInterface") > 0);
    CHECK(symMap["MyInterface"].kind == lsp::SymbolKind::Interface);

    CHECK(symMap.count("MyEnum") > 0);
    CHECK(symMap["MyEnum"].kind == lsp::SymbolKind::Enum);

    CHECK(symMap.count("MyTypedef") > 0);
    CHECK(symMap["MyTypedef"].kind == lsp::SymbolKind::Class);

    CHECK(symMap.count("MyFuncdef") > 0);
    CHECK(symMap["MyFuncdef"].kind == lsp::SymbolKind::Function);

    CHECK(symMap.count("myField") > 0);
    CHECK(symMap["myField"].kind == lsp::SymbolKind::Field);

    CHECK(symMap.count("myMethod") > 0);
    CHECK(symMap["myMethod"].kind == lsp::SymbolKind::Method);

    CHECK(symMap.count("myGlobalVar") > 0);
    CHECK(symMap["myGlobalVar"].kind == lsp::SymbolKind::Variable);

    CHECK(symMap.count("myGlobalFunc") > 0);
    CHECK(symMap["myGlobalFunc"].kind == lsp::SymbolKind::Function);
}

// =====================================================================================
// SECTION 3: ADVERSARIAL INCLUDE DIRECTIVE RESOLUTION TESTS
// =====================================================================================

TEST_CASE("Adversarial IncludeResolver - Deep Nested Linear Include Chain")
{
    TempDirGuard temp("inc_deep_chain");
    temp.WriteFile("1.as", "#include \"2.as\"");
    temp.WriteFile("2.as", "#include \"3.as\"");
    temp.WriteFile("3.as", "#include \"4.as\"");
    temp.WriteFile("4.as", "#include \"5.as\"");
    temp.WriteFile("5.as", "// leaf");

    std::string root = temp.PathString("1.as");
    auto all = IncludeResolver::ResolveAllIncludes(root, {});

    REQUIRE(all.size() == 4);
    CHECK(all[0] == temp.PathString("2.as"));
    CHECK(all[1] == temp.PathString("3.as"));
    CHECK(all[2] == temp.PathString("4.as"));
    CHECK(all[3] == temp.PathString("5.as"));
}

TEST_CASE("Adversarial IncludeResolver - Complex Multi-Node Cyclic Graph")
{
    TempDirGuard temp("inc_complex_cycle");
    // Graph:
    // A -> B, C
    // B -> D
    // C -> E
    // D -> C (cross cycle)
    // E -> A (cycle back to root)
    temp.WriteFile("A.as", "#include \"B.as\"\n#include \"C.as\"");
    temp.WriteFile("B.as", "#include \"D.as\"");
    temp.WriteFile("C.as", "#include \"E.as\"");
    temp.WriteFile("D.as", "#include \"C.as\"");
    temp.WriteFile("E.as", "#include \"A.as\"");

    std::string root = temp.PathString("A.as");
    auto all = IncludeResolver::ResolveAllIncludes(root, {});

    // Must resolve all 4 files B, C, D, E without infinite recursion
    REQUIRE(all.size() == 4);
    std::unordered_set<std::string> set(all.begin(), all.end());
    CHECK(set.count(temp.PathString("B.as")) == 1);
    CHECK(set.count(temp.PathString("C.as")) == 1);
    CHECK(set.count(temp.PathString("D.as")) == 1);
    CHECK(set.count(temp.PathString("E.as")) == 1);
}

TEST_CASE("Adversarial IncludeResolver - Angled vs Quoted Directives with Special Characters")
{
    std::string source =
        "#include <sys/core.h.as>\n"
        "#include \"user_dir/math-3d_v2.1.as\"\n"
        "   #   include   <a/b/c/d/e.as>   \n"
        "#include \"with spaces in name.as\"\n";

    auto includes = IncludeResolver::ExtractIncludes(source);
    REQUIRE(includes.size() == 4);

    CHECK(includes[0].rawPath == "sys/core.h.as");
    CHECK(includes[0].isAngled == true);
    CHECK(includes[0].line == 0);

    CHECK(includes[1].rawPath == "user_dir/math-3d_v2.1.as");
    CHECK(includes[1].isAngled == false);
    CHECK(includes[1].line == 1);

    CHECK(includes[2].rawPath == "a/b/c/d/e.as");
    CHECK(includes[2].isAngled == true);
    CHECK(includes[2].line == 2);

    CHECK(includes[3].rawPath == "with spaces in name.as");
    CHECK(includes[3].isAngled == false);
    CHECK(includes[3].line == 3);
}

TEST_CASE("Adversarial IncludeResolver - Missing Files in Middle of Chain")
{
    TempDirGuard temp("inc_missing_chain");
    // A -> B -> Missing -> C
    temp.WriteFile("A.as", "#include \"B.as\"");
    temp.WriteFile("B.as", "#include \"Missing.as\"\n#include \"C.as\"");
    temp.WriteFile("C.as", "// leaf");

    std::string root = temp.PathString("A.as");
    auto all = IncludeResolver::ResolveAllIncludes(root, {});

    // Should include B and C, gracefully skipping Missing.as without throwing
    REQUIRE(all.size() == 2);
    CHECK(all[0] == temp.PathString("B.as"));
    CHECK(all[1] == temp.PathString("C.as"));
}

TEST_CASE("Adversarial IncludeResolver - False Positives in Comments and String Literals")
{
    std::string source =
        "// Line comment: #include \"fake1.as\"\n"
        "/* Multi-line comment\n"
        "   #include \"fake2.as\"\n"
        "   #include <fake3.as>\n"
        "*/\n"
        "string s1 = \"#include \\\"fake4.as\\\"\";\n"
        "string s2 = @\"#include <fake5.as>\";\n"
        "string s3 = \"\"\" #include \"fake6.as\" \"\"\";\n"
        "char c = '#';\n"
        "#include \"real.as\"\n";

    auto includes = IncludeResolver::ExtractIncludes(source);
    REQUIRE(includes.size() == 1);
    CHECK(includes[0].rawPath == "real.as");
    CHECK(includes[0].line == 9);
}

TEST_CASE("Adversarial IncludeResolver - Relative vs Search Path Priority and Edge Paths")
{
    TempDirGuard temp("inc_priority_test");
    temp.WriteFile("src/entry.as", "#include \"config.as\"");
    temp.WriteFile("src/config.as", "// local config");
    temp.WriteFile("include/config.as", "// global config");

    std::string currentFile = temp.PathString("src/entry.as");
    std::vector<std::string> searchDirs = { temp.PathString("include") };

    // Local relative file MUST take precedence over searchDirectories
    std::string resolved = IncludeResolver::ResolveIncludePath("config.as", currentFile, searchDirs);
    CHECK(resolved == temp.PathString("src/config.as"));

    // Parent path navigation "../"
    temp.WriteFile("src/sub/nested.as", "#include \"../config.as\"");
    std::string nestedFile = temp.PathString("src/sub/nested.as");
    std::string resolvedParent = IncludeResolver::ResolveIncludePath("../config.as", nestedFile, searchDirs);
    CHECK(resolvedParent == temp.PathString("src/config.as"));

    // Empty search directory in list should be handled safely
    std::vector<std::string> searchDirsWithEmpty = { "", temp.PathString("include"), "" };
    std::string resolvedWithEmpty = IncludeResolver::ResolveIncludePath("config.as", currentFile, searchDirsWithEmpty);
    CHECK(resolvedWithEmpty == temp.PathString("src/config.as"));
}
