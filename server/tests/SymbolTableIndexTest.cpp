#include <doctest/doctest.h>

#include "analysis/SymbolTable.h"
#include "analysis/rules/RuleIndex.h"

#include <string>
#include <vector>

using namespace angel_lsp::analysis;

namespace
{
    Symbol MakeSymbol(SymbolType type, const std::string &name, const std::string &fileUri,
                      const std::string &containerName = "")
    {
        Symbol sym;
        sym.type = type;
        sym.name = name;
        sym.containerName = containerName;
        sym.qualifiedName = containerName.empty() ? name : containerName + "::" + name;
        sym.fileUri = fileUri;
        if (type == SymbolType::Function)
        {
            sym.signature = FunctionSignature{};
        }
        return sym;
    }

    /** @brief Names the per-file walk visits, in the buckets it visits. */
    std::vector<std::string> NamesInFile(const SymbolTable &table, const std::string &fileUri)
    {
        std::vector<std::string> names;
        table.ForEachSymbolInFile(fileUri,
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    names.push_back(sym.name);
                }
            });
        return names;
    }

    bool Contains(const std::vector<std::string> &names, const std::string &name)
    {
        return std::find(names.begin(), names.end(), name) != names.end();
    }
}

// =====================================================================================
// Per-file iteration
// =====================================================================================

TEST_CASE("SymbolTable - The per-file walk visits only that file's buckets")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Alpha", "file:///a.as"));
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Beta", "file:///b.as"));

    const auto fromA = NamesInFile(table, "file:///a.as");
    CHECK(fromA.size() == 1);
    CHECK(Contains(fromA, "Alpha"));
    CHECK_FALSE(Contains(fromA, "Beta"));
}

TEST_CASE("SymbolTable - A shared bucket is visited whole from either file")
{
    // The point of visiting buckets rather than symbols: the redeclaration rule compares a
    // document's declarations against every other declaration of the same name, and a sibling file
    // of the same module is exactly where the interesting collisions come from.
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Update", "file:///a.as"));
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Update", "file:///b.as"));

    const auto fromA = NamesInFile(table, "file:///a.as");
    CHECK(fromA.size() == 2);
}

TEST_CASE("SymbolTable - An unknown file visits nothing")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Alpha", "file:///a.as"));

    CHECK(NamesInFile(table, "file:///never-indexed.as").empty());
}

TEST_CASE("SymbolTable - Clearing a document removes it from the per-file walk")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Alpha", "file:///a.as"));
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Beta", "file:///b.as"));

    table.ClearDocumentSymbols("file:///a.as");

    CHECK(NamesInFile(table, "file:///a.as").empty());
    CHECK(Contains(NamesInFile(table, "file:///b.as"), "Beta"));
    CHECK_FALSE(table.HasSymbol("Alpha"));
}

TEST_CASE("SymbolTable - Clearing one file leaves a bucket the other still shares")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Update", "file:///a.as"));
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Update", "file:///b.as"));

    table.ClearDocumentSymbols("file:///a.as");

    CHECK(table.HasSymbol("Update"));
    CHECK(NamesInFile(table, "file:///b.as").size() == 1);
}

TEST_CASE("SymbolTable - Replacing a document re-indexes it")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Before", "file:///a.as"));

    SymbolTable staging;
    staging.AddSymbol(MakeSymbol(SymbolType::Class, "After", "file:///a.as"));
    table.ReplaceDocumentSymbols("file:///a.as", staging);

    const auto names = NamesInFile(table, "file:///a.as");
    CHECK(Contains(names, "After"));
    CHECK_FALSE(Contains(names, "Before"));
    CHECK_FALSE(table.HasSymbol("Before"));
}

// =====================================================================================
// Index invalidation
// =====================================================================================

TEST_CASE("SymbolTable - The rule index is reused while the table is unchanged")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Class, "Alpha", "file:///a.as"));

    CHECK((table.GetRuleIndex().get() == table.GetRuleIndex().get()));
}

TEST_CASE("SymbolTable - The rule index is rebuilt after a write")
{
    // The failure this guards is silent and nasty: a cached index outliving the edit that
    // invalidated it means rules answering questions about symbols that no longer exist.
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Think", "file:///a.as", "Entity"));

    const auto before = table.GetRuleIndex();
    CHECK(before->Members("Entity").methodNames.contains("Think"));
    CHECK(before->allNames.contains("Think"));

    table.AddSymbol(MakeSymbol(SymbolType::Function, "Spawn", "file:///a.as", "Entity"));

    const auto after = table.GetRuleIndex();
    CHECK((after.get() != before.get()));
    CHECK(after->Members("Entity").methodNames.contains("Spawn"));
    CHECK(after->allNames.contains("Spawn"));
}

TEST_CASE("SymbolTable - Clearing a document invalidates the rule index")
{
    SymbolTable table;
    table.AddSymbol(MakeSymbol(SymbolType::Function, "Think", "file:///a.as", "Entity"));
    CHECK(table.GetRuleIndex()->allNames.contains("Think"));

    table.ClearDocumentSymbols("file:///a.as");

    CHECK_FALSE(table.GetRuleIndex()->allNames.contains("Think"));
}

TEST_CASE("SymbolTable - The version moves on every write and holds still otherwise")
{
    SymbolTable table;
    const uint64_t initial = table.Version();

    table.AddSymbol(MakeSymbol(SymbolType::Class, "Alpha", "file:///a.as"));
    const uint64_t afterAdd = table.Version();
    CHECK(afterAdd != initial);

    CHECK(table.Version() == afterAdd);

    table.ClearDocumentSymbols("file:///a.as");
    CHECK(table.Version() != afterAdd);
}
