#include <doctest/doctest.h>

#include "features/completion/CompletionHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "config/ServerConfig.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    struct TestEnvironment
    {
        AngelScriptParser parser;
        SymbolCollector symbolCollector{ nullptr };
        LocalScopeCollector scopeCollector{ nullptr };
        SymbolTable symbolTable;
        ScopeIndex scopeIndex;
        angel_lsp::i18n::I18n i18n;
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        /**
         * @param runAnalyzer Mirror the server's own order - collect scopes, analyse the *unpublished*
         *                    tree, then publish it. Some completions depend on a type the analyzer
         *                    deduces and writes back: `auto`, and a foreach variable's element type.
         *                    Skipping this step is why a test can see an empty member list for a
         *                    name the running server completes perfectly well
         *                    (Server::CollectScopesAndAnalyze).
         */
        TestEnvironment(const std::string &code, bool runAnalyzer = false)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            std::shared_ptr<Scope> rootScope = scopeCollector.CollectScopes(sourceCode, parser);

            if (rootScope && runAnalyzer)
            {
                angel_lsp::config::TypeConfig types;
                SemanticAnalysisRequest request{ symbolTable, uri, ".as.predefined", &i18n };
                request.typeConfig = &types;
                request.sourceCode = sourceCode;
                request.tree = tree;
                request.scopeRoot = rootScope;
                // The caller still owns this tree exclusively, which is what makes the write-back
                // sound - see TypeConversionChecker.h on mutableScopeRoot.
                request.mutableScopeRoot = rootScope.get();

                SemanticAnalyzer analyzer(nullptr);
                analyzer.Analyze(request);
            }

            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::shared_ptr<const Scope>(std::move(rootScope)));
            }
        }

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::vector<lsp::CompletionItem> CompleteAt(uint32_t line, uint32_t character,
                                                    bool snippetSupport = false)
        {
            CompletionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex,
                                   lsp::Position{ line, character }, nullptr, snippetSupport };
            return GetCompletion(req);
        }
    };

    bool HasItem(const std::vector<lsp::CompletionItem> &items, const std::string &label)
    {
        for (const auto &item : items)
        {
            if (item.label == label)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("CompletionHandler - Member Access Completion")
{
    std::string code = 
        "class Player {\n"
        "    int health;\n"
        "    void Jump() {}\n"
        "}\n"
        "void main() {\n"
        "    Player p;\n"
        "    p.\n"
        "}\n";

    TestEnvironment env(code);
    auto items = env.CompleteAt(6, 6); // right after 'p.'
    
    CHECK(HasItem(items, "health"));
    CHECK(HasItem(items, "Jump"));
    // Unrelated keywords / globals shouldn't pollute member access
    CHECK(!HasItem(items, "while"));
    CHECK(!HasItem(items, "for"));
}

TEST_CASE("CompletionHandler - Scope Resolution Completion")
{
    std::string code = 
        "enum State {\n"
        "    Idle,\n"
        "    Running,\n"
        "    Jumping\n"
        "}\n"
        "void main() {\n"
        "    State::\n"
        "}\n";

    TestEnvironment env(code);
    auto items = env.CompleteAt(6, 11); // right after 'State::'

    CHECK(HasItem(items, "Idle"));
    CHECK(HasItem(items, "Running"));
    CHECK(HasItem(items, "Jumping"));
}

TEST_CASE("CompletionHandler - Global and Lexical Scope Completion")
{
    std::string code = 
        "int g_globalVar = 10;\n"
        "void GlobalFunc() {}\n"
        "void main() {\n"
        "    int localVar = 5;\n"
        "    \n"
        "}\n";

    TestEnvironment env(code);
    auto items = env.CompleteAt(4, 4); // inside main body

    CHECK(HasItem(items, "localVar"));
    CHECK(HasItem(items, "g_globalVar"));
    CHECK(HasItem(items, "GlobalFunc"));
    CHECK(HasItem(items, "int"));
    CHECK(HasItem(items, "return"));
    CHECK(HasItem(items, "if"));
}

TEST_CASE("CompletionHandler - Symbol items carry the identity a resolve needs")
{
    const std::string code =
        "/// Spawns the entity at its start position.\n"
        "void Spawn(int id) {}\n"
        "void main() { Sp }\n";

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    LocalScopeCollector scopes(nullptr);
    SymbolTable table;
    ScopeIndex index;
    const std::string uri = "file:///resolve.as";

    TSTree *tree = parser.Parse(code);
    collector.CollectSymbols(uri, code, parser, table);
    if (auto root = scopes.CollectScopes(code, parser))
    {
        index.SetScopeTree(uri, std::move(root));
    }

    CompletionRequest request{ uri, code, tree, table, index, lsp::Position{ 2, 16 }, nullptr };
    const auto items = GetCompletion(request);

    const auto spawn = std::find_if(items.begin(), items.end(),
                                    [](const lsp::CompletionItem &item) { return item.label == "Spawn"; });
    REQUIRE(spawn != items.end());

    // Nothing is resolved yet: the list is cheap on purpose.
    REQUIRE(spawn->data.has_value());
    CHECK(spawn->data->isString());
    CHECK(spawn->data->string() == "Spawn");
    CHECK_FALSE(spawn->documentation.has_value());

    ts_tree_delete(tree);
}

TEST_CASE("CompletionHandler - Resolve attaches the declaration's doc comment")
{
    const std::string code =
        "/// Spawns the entity at its start position.\n"
        "void Spawn(int id) {}\n";

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    SymbolTable table;
    const std::string uri = "file:///resolve.as";
    collector.CollectSymbols(uri, code, parser, table);

    lsp::CompletionItem item;
    item.label = "Spawn";
    item.data = lsp::LSPAny(std::string("Spawn"));

    CompletionResolveRequest request{
        item,
        table,
        [&](const std::string &wanted) -> const std::string * { return wanted == uri ? &code : nullptr; }
    };

    const auto resolved = ResolveCompletionItem(request);
    REQUIRE(resolved.documentation.has_value());
    const auto &markup = std::get<lsp::MarkupContent>(*resolved.documentation);
    CHECK(markup.value.find("Spawns the entity") != std::string::npos);
}

TEST_CASE("CompletionHandler - Resolve leaves an item it cannot identify alone")
{
    SymbolTable table;

    SUBCASE("No data at all, as on a keyword item")
    {
        lsp::CompletionItem item;
        item.label = "while";

        CompletionResolveRequest request{
            item, table, [](const std::string &) -> const std::string * { return nullptr; } };

        CHECK_FALSE(ResolveCompletionItem(request).documentation.has_value());
    }

    SUBCASE("Names a symbol the table does not have")
    {
        lsp::CompletionItem item;
        item.label = "Ghost";
        item.data = lsp::LSPAny(std::string("Ghost"));

        CompletionResolveRequest request{
            item, table, [](const std::string &) -> const std::string * { return nullptr; } };

        CHECK_FALSE(ResolveCompletionItem(request).documentation.has_value());
    }
}

TEST_CASE("CompletionHandler - Resolve keeps documentation an item already had")
{
    const std::string code = "/// Fresh text.\nvoid Spawn() {}\n";

    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    SymbolTable table;
    collector.CollectSymbols("file:///resolve.as", code, parser, table);

    lsp::CompletionItem item;
    item.label = "Spawn";
    item.data = lsp::LSPAny(std::string("Spawn"));
    item.documentation = lsp::MarkupContent{ lsp::MarkupKindEnum(lsp::MarkupKind::Markdown), "Already known." };

    CompletionResolveRequest request{
        item, table, [&](const std::string &) -> const std::string * { return &code; } };

    const auto resolved = ResolveCompletionItem(request);
    REQUIRE(resolved.documentation.has_value());
    CHECK(std::get<lsp::MarkupContent>(*resolved.documentation).value == "Already known.");
}

// =====================================================================================
// Constructors and destructors are not members you can call on an instance.
//
// AngelScript has no syntax for it: `m.Matrix()` is rejected by the real compiler with
// "No matching symbol 'Matrix'", and `Matrix m.Matrix();` is not even parseable. Verified against
// asharness. Offering them after `m.` therefore invites the user to write something that cannot
// compile - and because a constructor is stored as an ordinary Function symbol named after its
// class (`Matrix::Matrix`), nothing filtered them out.
// =====================================================================================

TEST_CASE("Completion - Constructors and destructors are not offered as instance members")
{
    const std::string code =
        "class Matrix\n"                 // 0
        "{\n"                            // 1
        "    Matrix() {}\n"              // 2
        "    Matrix(int rows) {}\n"      // 3
        "    ~Matrix() {}\n"             // 4
        "    int Rows() { return 0; }\n" // 5
        "    int size;\n"                // 6
        "}\n"                            // 7
        "void main()\n"                  // 8
        "{\n"                            // 9
        "    Matrix m;\n"                // 10
        "    m.\n"                       // 11
        "}\n";                           // 12

    TestEnvironment env(code);
    const auto items = env.CompleteAt(11, 6);

    // The real members are still there - this must not have become a filter that removes everything.
    CHECK(HasItem(items, "Rows"));
    CHECK(HasItem(items, "size"));

    CHECK_FALSE(HasItem(items, "Matrix"));
    CHECK_FALSE(HasItem(items, "~Matrix"));
}

// =====================================================================================
// Template arguments.
//
// `array` on its own is not a type - it is a template that needs its argument written out, and the
// real compiler says so - so completing it to the bare name hands the user something that cannot
// compile. Inside the brackets the opposite problem applied: only a type may be written there, and
// completion was offering the entire lexical scope, keywords and local variables included.
// =====================================================================================

namespace
{
    const lsp::CompletionItem *FindItem(const std::vector<lsp::CompletionItem> &items,
                                        const std::string &label)
    {
        for (const auto &item : items)
        {
            if (item.label == label)
            {
                return &item;
            }
        }
        return nullptr;
    }
}

TEST_CASE("Completion - A template class completes with its argument list")
{
    // The declarations come after `main` so the cursor is not inside any class body: a class the
    // cursor sits within is completed through a different path, which would make this test measure
    // the wrong one.
    const std::string code =
        "void main()\n"       // 0
        "{\n"                 // 1
        "    arr\n"           // 2
        "}\n"                 // 3
        "class array<T>\n"    // 4
        "{\n"                 // 5
        "    uint length();\n"// 6
        "}\n";                // 7

    TestEnvironment env(code);

    const auto items = env.CompleteAt(2, 7, /*snippetSupport=*/true);
    const lsp::CompletionItem *item = FindItem(items, "array");
    REQUIRE(item != nullptr);
    REQUIRE(item->insertText.has_value());
    CHECK(item->insertText.value() == "array<${1:T}>$0");
    REQUIRE(item->insertTextFormat.has_value());
    CHECK(item->insertTextFormat.value() == lsp::InsertTextFormat::Snippet);

    // A client that did not advertise snippet support gets the plain name instead of six
    // characters of placeholder syntax printed into its buffer.
    const auto plain = env.CompleteAt(2, 7, /*snippetSupport=*/false);
    const lsp::CompletionItem *plainItem = FindItem(plain, "array");
    REQUIRE(plainItem != nullptr);
    CHECK_FALSE(plainItem->insertText.has_value());
}

TEST_CASE("Completion - Inside template brackets only types are offered")
{
    const std::string code =
        "void main()\n"          // 0
        "{\n"                    // 1
        "    int counter = 0;\n" // 2
        "    array<\n"           // 3
        "}\n"                    // 4
        "class array<T> {}\n"    // 5
        "class Vector {}\n"      // 6
        "enum Team { Red }\n"    // 7
        "void Helper() {}\n";    // 8

    TestEnvironment env(code);
    const auto items = env.CompleteAt(3, 10);

    CHECK(HasItem(items, "Vector"));
    CHECK(HasItem(items, "Team"));
    CHECK(HasItem(items, "int"));

    // None of these may be written as a template argument.
    CHECK_FALSE(HasItem(items, "counter"));
    CHECK_FALSE(HasItem(items, "Helper"));
    CHECK_FALSE(HasItem(items, "while"));
}

TEST_CASE("Completion - A less-than comparison is not a template argument list")
{
    // The whole risk of the backwards scan is reading `a < b` as an unclosed argument list and
    // then refusing to offer the local the user is halfway through typing.
    const std::string code =
        "void main()\n"          // 0
        "{\n"                    // 1
        "    int counter = 0;\n" // 2
        "    if (counter < c\n"  // 3
        "}\n";

    TestEnvironment env(code);
    const auto items = env.CompleteAt(3, 19);
    CHECK(HasItem(items, "counter"));
}

TEST_CASE("Completion - A closed argument list is not still open")
{
    const std::string code =
        "void main()\n"                       // 0
        "{\n"                                 // 1
        "    array<array<int>> grid;\n"       // 2
        "    gr\n"                            // 3
        "}\n"                                 // 4
        "class array<T> {}\n";                // 5

    TestEnvironment env(code);
    const auto items = env.CompleteAt(3, 6);
    CHECK(HasItem(items, "grid"));
}

// =====================================================================================
// foreach loop variables.
//
// `foreach (auto v : items)` writes `auto` and nothing else, so the element type has to come from
// the container. AngelScript drives the loop through `opForValue<N>`, and the Nth loop variable
// takes that method's return type - which the stub already declares, so no per-type knowledge is
// needed. Verified against asharness: `foreach (auto v : array<string>)` lets `v.length()` compile,
// and `foreach (auto n : anInt)` is rejected with "Type 'int' is not valid type for foreach loops".
// =====================================================================================

TEST_CASE("Completion - A foreach variable completes as the container's element type")
{
    const std::string code =
        "void main()\n"                          // 0
        "{\n"                                    // 1
        "    array<Item> items;\n"               // 2
        "    foreach (auto entry : items)\n"     // 3
        "    {\n"                                // 4
        "        entry.\n"                       // 5
        "    }\n"                                // 6
        "}\n"                                    // 7
        "class Item { int health; void Spawn() {} }\n"  // 8
        "class array<T>\n"                       // 9
        "{\n"                                    // 10
        "    const T& opForValue0(uint index) const;\n"  // 11
        "    uint opForValue1(uint index) const;\n"      // 12
        "}\n";                                   // 13

    TestEnvironment env(code, /*runAnalyzer=*/true);
    const auto items = env.CompleteAt(5, 14);

    CHECK(HasItem(items, "health"));
    CHECK(HasItem(items, "Spawn"));
}

TEST_CASE("Completion - The second foreach variable is the index, not the element")
{
    const std::string code =
        "void main()\n"                                  // 0
        "{\n"                                            // 1
        "    array<Item> items;\n"                       // 2
        "    foreach (auto entry, auto index : items)\n" // 3
        "    {\n"                                        // 4
        "        index.\n"                               // 5
        "    }\n"                                        // 6
        "}\n"                                            // 7
        "class Item { int health; }\n"                   // 8
        "class array<T>\n"                               // 9
        "{\n"                                            // 10
        "    const T& opForValue0(uint index) const;\n"  // 11
        "    uint opForValue1(uint index) const;\n"      // 12
        "}\n";                                           // 13

    TestEnvironment env(code, /*runAnalyzer=*/true);
    const auto items = env.CompleteAt(5, 14);

    // `opForValue1` returns uint, so the index must not carry the element's members.
    CHECK_FALSE(HasItem(items, "health"));
}
