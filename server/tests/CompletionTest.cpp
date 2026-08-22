#include <doctest/doctest.h>

#include "features/completion/CompletionHandler.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "parser/AngelScriptParser.h"

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
        std::string uri = "file:///test.as";
        std::string sourceCode;
        TSTree *tree = nullptr;

        TestEnvironment(const std::string &code)
            : sourceCode(code)
        {
            tree = parser.Parse(sourceCode);
            symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
            auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
            if (rootScope)
            {
                scopeIndex.SetScopeTree(uri, std::move(rootScope));
            }
        }

        ~TestEnvironment()
        {
            if (tree)
            {
                ts_tree_delete(tree);
            }
        }

        std::vector<lsp::CompletionItem> CompleteAt(uint32_t line, uint32_t character)
        {
            CompletionRequest req{ uri, sourceCode, tree, symbolTable, scopeIndex, lsp::Position{ line, character } };
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
