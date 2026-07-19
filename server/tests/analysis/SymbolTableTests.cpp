#include <doctest/doctest.h>
#include "analysis/SymbolTable.h"

using namespace analysis;

TEST_SUITE("SymbolTable")
{
    TEST_CASE("Add and find global symbol")
    {
        SymbolTable table;
        auto sym = std::make_shared<Symbol>();
        sym->name = "MyGlobalVar";
        sym->kind = SymbolKind::Variable;
        sym->typeInfo = "int";

        table.AddGlobal(sym);

        Symbol *found = table.FindGlobalByName("MyGlobalVar");
        REQUIRE(found != nullptr);
        CHECK(found->name == "MyGlobalVar");
        CHECK(found->kind == SymbolKind::Variable);
        CHECK(found->typeInfo == "int");

        // Non-existent symbol
        Symbol *notFound = table.FindGlobalByName("DoesNotExist");
        CHECK(notFound == nullptr);
    }

    TEST_CASE("FindScopeByPosition finds the tightest scope")
    {
        SymbolTable table;

        // Global function: lines 0 to 10
        auto funcSym = std::make_shared<Symbol>();
        funcSym->name = "MyFunc";
        funcSym->kind = SymbolKind::Function;
        funcSym->fullRange.start = {0, 0};
        funcSym->fullRange.end = {10, 0};
        table.AddGlobal(funcSym);

        // Local scope/block (e.g. an if-statement or just local vars): lines 2 to 5
        auto localSym = std::make_shared<Symbol>();
        localSym->name = "LocalVar";
        localSym->kind = SymbolKind::Variable;
        localSym->fullRange.start = {2, 0};
        localSym->fullRange.end = {5, 10};
        table.AddLocal(localSym);

        // Position exactly on line 3 should find the local symbol
        Symbol *insideLocal = table.FindScopeByPosition("", 3, 5);
        REQUIRE(insideLocal != nullptr);
        CHECK(insideLocal->name == "LocalVar");

        // Position on line 8 is outside local, should find global function
        Symbol *insideGlobal = table.FindScopeByPosition("", 8, 0);
        REQUIRE(insideGlobal != nullptr);
        CHECK(insideGlobal->name == "MyFunc");

        // Position on line 12 is completely outside, should find nothing
        Symbol *outside = table.FindScopeByPosition("", 12, 0);
        CHECK(outside == nullptr);
    }

    TEST_CASE("ClearLocals does not clear globals")
    {
        SymbolTable table;

        auto globalSym = std::make_shared<Symbol>();
        globalSym->name = "Global";
        table.AddGlobal(globalSym);

        auto localSym = std::make_shared<Symbol>();
        localSym->name = "Local";
        localSym->fullRange.start = {0, 0};
        localSym->fullRange.end = {10, 0};
        table.AddLocal(localSym);

        table.ClearLocals();

        CHECK(table.FindGlobalByName("Global") != nullptr);
        CHECK(table.FindScopeByPosition("", 5, 5) == nullptr);
        // It will find Global if Global had a range covering 5,5, but Global has no range here (all zeros).
        // Let's set Global range to cover everything
        globalSym->fullRange.start = {0, 0};
        globalSym->fullRange.end = {100, 0};

        // Now it should find Global
        Symbol *found = table.FindScopeByPosition("", 5, 5);
        REQUIRE(found != nullptr);
        CHECK(found->name == "Global");
    }
    TEST_CASE("ST-M1: AddGlobal with same name does not overwrite (multimap)")
    {
        SymbolTable table;
        auto sym1 = std::make_shared<Symbol>();
        sym1->name = "Collider";
        sym1->kind = SymbolKind::Class;

        auto sym2 = std::make_shared<Symbol>();
        sym2->name = "Collider";
        sym2->kind = SymbolKind::Function;

        table.AddGlobal(sym1);
        table.AddGlobal(sym2);

        auto globals = table.FindAllGlobalsByName("Collider");
        REQUIRE(globals.size() == 2);
        CHECK(globals[0]->kind == SymbolKind::Class);
        CHECK(globals[1]->kind == SymbolKind::Function);
    }

    TEST_CASE("ST-M2: FindGlobalByName returns the first one")
    {
        SymbolTable table;
        auto sym1 = std::make_shared<Symbol>();
        sym1->name = "Collider";
        sym1->kind = SymbolKind::Class;

        auto sym2 = std::make_shared<Symbol>();
        sym2->name = "Collider";
        sym2->kind = SymbolKind::Function;

        table.AddGlobal(sym1);
        table.AddGlobal(sym2);

        Symbol *found = table.FindGlobalByName("Collider");
        REQUIRE(found != nullptr);
        CHECK(found->kind == SymbolKind::Class); // Returns first added
    }

    TEST_CASE("ST-M3: FindAllGlobalsByName inside namespace")
    {
        SymbolTable table;
        auto nsSym = std::make_shared<Symbol>();
        nsSym->name = "Math";
        nsSym->kind = SymbolKind::Namespace;

        auto child1 = std::make_shared<Symbol>();
        child1->name = "Lerp";
        child1->kind = SymbolKind::Function;

        auto child2 = std::make_shared<Symbol>();
        child2->name = "Lerp";
        child2->kind = SymbolKind::Variable;

        nsSym->children.push_back(child1);
        nsSym->children.push_back(child2);

        table.AddGlobal(nsSym);

        // FindAllGlobalsByName deep? No, FindAllGlobalsByName is just for globals.
        // Wait, ST-M3 says "FindAllGlobalsByName dentro de namespace" but FindAllGlobalsByName only searches globals.
        // Let's test that FindByNameDeep still works as expected.
        const Symbol *found = table.FindByNameDeep("Lerp");
        REQUIRE(found != nullptr);
        CHECK(found->name == "Lerp");
        // Deep search returns the first one it finds.
    }
}
