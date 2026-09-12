#include <doctest/doctest.h>

#include "analysis/SymbolTable.h"
#include "analysis/rules/RuleIndex.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::analysis::rules;

namespace
{
    /**
     * @brief Creates a helper Symbol for testing.
     */
    Symbol MakeTestSymbol(SymbolType type,
                          const std::string &name,
                          const std::string &fileUri,
                          const std::string &containerName = "")
    {
        Symbol sym;
        sym.type = type;
        sym.name = name;
        sym.containerName = containerName;
        sym.qualifiedName = containerName.empty() ? name : containerName + "::" + name;
        sym.fileUri = fileUri;

        switch (type)
        {
        case SymbolType::Function:
            sym.signature = FunctionSignature{};
            break;
        case SymbolType::Class:
            sym.signature = ClassSignature{};
            break;
        case SymbolType::Interface:
            sym.signature = InterfaceSignature{};
            break;
        case SymbolType::Enum:
            sym.signature = EnumSignature{};
            break;
        case SymbolType::Variable:
            sym.signature = VariableSignature{};
            break;
        default:
            break;
        }
        return sym;
    }

    /**
     * @brief Asserts 100% equivalence between an incremental RuleIndex and an oracle rebuild.
     */
    void AssertRuleIndexEquivalence(const RuleIndex &actual, const RuleIndex &oracle)
    {
        // 1. allNames
        CHECK(actual.allNames.size() == oracle.allNames.size());
        for (const auto &[name, count] : oracle.allNames)
        {
            auto it = actual.allNames.find(name);
            CHECK(it != actual.allNames.end());
            if (it != actual.allNames.end())
            {
                CHECK(it->second == count);
            }
        }

        // 2. enumMemberNames & enumMemberCounts
        CHECK(actual.enumMemberNames.size() == oracle.enumMemberNames.size());
        for (const auto &name : oracle.enumMemberNames)
        {
            CHECK(actual.enumMemberNames.contains(name));
        }
        CHECK(actual.enumMemberCounts.size() == oracle.enumMemberCounts.size());
        for (const auto &[name, count] : oracle.enumMemberCounts)
        {
            auto it = actual.enumMemberCounts.find(name);
            CHECK(it != actual.enumMemberCounts.end());
            if (it != actual.enumMemberCounts.end())
            {
                CHECK(it->second == count);
            }
        }

        // 3. accessorPropertyNames & accessorPropertyCounts
        CHECK(actual.accessorPropertyNames.size() == oracle.accessorPropertyNames.size());
        for (const auto &name : oracle.accessorPropertyNames)
        {
            CHECK(actual.accessorPropertyNames.contains(name));
        }

        // 4. keywordAccessorPropertyNames & keywordAccessorPropertyCounts
        CHECK(actual.keywordAccessorPropertyNames.size() == oracle.keywordAccessorPropertyNames.size());
        for (const auto &name : oracle.keywordAccessorPropertyNames)
        {
            CHECK(actual.keywordAccessorPropertyNames.contains(name));
        }
        CHECK(actual.keywordAccessorPropertyCounts.size() == oracle.keywordAccessorPropertyCounts.size());
        for (const auto &[name, count] : oracle.keywordAccessorPropertyCounts)
        {
            auto it = actual.keywordAccessorPropertyCounts.find(name);
            CHECK(it != actual.keywordAccessorPropertyCounts.end());
            if (it != actual.keywordAccessorPropertyCounts.end())
            {
                CHECK(it->second == count);
            }
        }

        // 4b. enumSymbolsByMemberName
        CHECK(actual.enumSymbolsByMemberName.size() == oracle.enumSymbolsByMemberName.size());
        for (const auto &[name, oracleSymbols] : oracle.enumSymbolsByMemberName)
        {
            auto it = actual.enumSymbolsByMemberName.find(name);
            CHECK(it != actual.enumSymbolsByMemberName.end());
            if (it != actual.enumSymbolsByMemberName.end())
            {
                CHECK(it->second.size() == oracleSymbols.size());
            }
        }

        // 4c. qualifiedTypesByShortName & qualifiedTypeCounts
        CHECK(actual.qualifiedTypesByShortName.size() == oracle.qualifiedTypesByShortName.size());
        for (const auto &[shortName, oracleTypes] : oracle.qualifiedTypesByShortName)
        {
            auto it = actual.qualifiedTypesByShortName.find(shortName);
            CHECK(it != actual.qualifiedTypesByShortName.end());
            if (it != actual.qualifiedTypesByShortName.end())
            {
                CHECK(it->second.size() == oracleTypes.size());
            }
        }
        CHECK(actual.qualifiedTypeCounts.size() == oracle.qualifiedTypeCounts.size());
        for (const auto &[shortName, oracleCounts] : oracle.qualifiedTypeCounts)
        {
            auto it = actual.qualifiedTypeCounts.find(shortName);
            CHECK(it != actual.qualifiedTypeCounts.end());
            if (it != actual.qualifiedTypeCounts.end())
            {
                CHECK(it->second == oracleCounts);
            }
        }

        // 5. derivedByBase
        CHECK(actual.derivedByBase.size() == oracle.derivedByBase.size());
        for (const auto &[base, oracleDerived] : oracle.derivedByBase)
        {
            auto it = actual.derivedByBase.find(base);
            CHECK(it != actual.derivedByBase.end());
            if (it != actual.derivedByBase.end())
            {
                CHECK(it->second.size() == oracleDerived.size());
                for (const auto &d : oracleDerived)
                {
                    bool found = false;
                    for (const auto &act : it->second)
                    {
                        if (act.qualifiedName == d.qualifiedName && act.name == d.name)
                        {
                            found = true;
                            break;
                        }
                    }
                    CHECK(found);
                }
            }
        }

        // 6. hostClassesByMixin
        CHECK(actual.hostClassesByMixin.size() == oracle.hostClassesByMixin.size());
        for (const auto &[mixin, oracleHosts] : oracle.hostClassesByMixin)
        {
            auto it = actual.hostClassesByMixin.find(mixin);
            CHECK(it != actual.hostClassesByMixin.end());
            if (it != actual.hostClassesByMixin.end())
            {
                CHECK(it->second.size() == oracleHosts.size());
                for (const auto &h : oracleHosts)
                {
                    bool found = false;
                    for (const auto &act : it->second)
                    {
                        if (act.qualifiedName == h.qualifiedName && act.name == h.name)
                        {
                            found = true;
                            break;
                        }
                    }
                    CHECK(found);
                }
            }
        }

        // 7. byContainer
        CHECK(actual.byContainer.size() == oracle.byContainer.size());
        for (const auto &[containerName, oracleCm] : oracle.byContainer)
        {
            auto it = actual.byContainer.find(containerName);
            CHECK(it != actual.byContainer.end());
            if (it != actual.byContainer.end())
            {
                const auto &actCm = it->second;
                CHECK(actCm.methodNames == oracleCm.methodNames);
                CHECK(actCm.finalMethodNames == oracleCm.finalMethodNames);
                CHECK(actCm.allMemberNames == oracleCm.allMemberNames);
                CHECK(actCm.nestedTypeCount == oracleCm.nestedTypeCount);
                CHECK(actCm.hasNestedType == oracleCm.hasNestedType);
                CHECK(actCm.memberKeySet == oracleCm.memberKeySet);
                CHECK(actCm.methodCounts == oracleCm.methodCounts);
                CHECK(actCm.finalMethodCounts == oracleCm.finalMethodCounts);
                CHECK(actCm.allMemberCounts == oracleCm.allMemberCounts);
                CHECK(actCm.memberKeyCounts == oracleCm.memberKeyCounts);
            }
        }
    }
}

TEST_CASE("RuleIndex - Equivalence with Oracle on Incremental Additions and Removals")
{
    SymbolTable table;

    // Document A: declares class Weapon with methods Shoot and Reload
    std::vector<Symbol> docA;
    docA.push_back(MakeTestSymbol(SymbolType::Class, "Weapon", "file:///weapon.as"));
    docA.push_back(MakeTestSymbol(SymbolType::Function, "Shoot", "file:///weapon.as", "Weapon"));
    docA.push_back(MakeTestSymbol(SymbolType::Function, "Reload", "file:///weapon.as", "Weapon"));
    {
        SymbolTable staging;
        for (const auto &sym : docA)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///weapon.as", staging);
    }

    auto incremental1 = table.GetRuleIndex();
    auto oracle1 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incremental1, *oracle1);

    // Document B: declares class Player with method Move
    std::vector<Symbol> docB;
    docB.push_back(MakeTestSymbol(SymbolType::Class, "Player", "file:///player.as"));
    docB.push_back(MakeTestSymbol(SymbolType::Function, "Move", "file:///player.as", "Player"));
    {
        SymbolTable staging;
        for (const auto &sym : docB)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///player.as", staging);
    }

    auto incremental2 = table.GetRuleIndex();
    auto oracle2 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incremental2, *oracle2);

    // Clear Document A
    table.ClearDocumentSymbols("file:///weapon.as");
    auto incremental3 = table.GetRuleIndex();
    auto oracle3 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incremental3, *oracle3);
    CHECK_FALSE(incremental3->byContainer.contains("Weapon"));
    CHECK(incremental3->byContainer.contains("Player"));
}

TEST_CASE("RuleIndex - Equivalence when Mixin Added AFTER Host Class")
{
    SymbolTable table;

    // 1. Add host class Rifle in File H that specifies base "WeaponMixin"
    {
        std::vector<Symbol> docH;
        Symbol hostSym = MakeTestSymbol(SymbolType::Class, "Rifle", "file:///rifle.as");
        hostSym.GetClass().bases.push_back("WeaponMixin");
        docH.push_back(hostSym);
        docH.push_back(MakeTestSymbol(SymbolType::Function, "Fire", "file:///rifle.as", "Rifle"));

        SymbolTable staging;
        for (const auto &sym : docH)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///rifle.as", staging);
    }

    // At this point, WeaponMixin is not in the table, so hostClassesByMixin is empty
    auto incBefore = table.GetRuleIndex();
    auto orcBefore = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incBefore, *orcBefore);
    CHECK_FALSE(incBefore->hostClassesByMixin.contains("WeaponMixin"));

    // 2. Add mixin class WeaponMixin in File M
    {
        std::vector<Symbol> docM;
        Symbol mixinSym = MakeTestSymbol(SymbolType::Class, "WeaponMixin", "file:///weapon_mixin.as");
        mixinSym.GetClass().modifiers.isMixin = true;
        docM.push_back(mixinSym);
        docM.push_back(MakeTestSymbol(SymbolType::Function, "GetAmmo", "file:///weapon_mixin.as", "WeaponMixin"));

        SymbolTable staging;
        for (const auto &sym : docM)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///weapon_mixin.as", staging);
    }

    // Now WeaponMixin is resolved, Rifle has includedMixins = {"WeaponMixin"},
    // and hostClassesByMixin["WeaponMixin"] should contain Rifle!
    auto incAfter = table.GetRuleIndex();
    auto orcAfter = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incAfter, *orcAfter);
    CHECK(incAfter->hostClassesByMixin.contains("WeaponMixin"));
    CHECK(incAfter->hostClassesByMixin.at("WeaponMixin").size() == 1);
    CHECK(incAfter->hostClassesByMixin.at("WeaponMixin")[0].name == "Rifle");

    // 3. Clear the mixin file
    table.ClearDocumentSymbols("file:///weapon_mixin.as");
    auto incCleared = table.GetRuleIndex();
    auto orcCleared = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*incCleared, *orcCleared);
    CHECK_FALSE(incCleared->hostClassesByMixin.contains("WeaponMixin"));
}

TEST_CASE("RuleIndex - Equivalence when Mixin Added BEFORE Host Class")
{
    SymbolTable table;

    // 1. Add mixin class WeaponMixin first
    {
        std::vector<Symbol> docM;
        Symbol mixinSym = MakeTestSymbol(SymbolType::Class, "WeaponMixin", "file:///weapon_mixin.as");
        mixinSym.GetClass().modifiers.isMixin = true;
        docM.push_back(mixinSym);
        docM.push_back(MakeTestSymbol(SymbolType::Function, "GetAmmo", "file:///weapon_mixin.as", "WeaponMixin"));

        SymbolTable staging;
        for (const auto &sym : docM)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///weapon_mixin.as", staging);
    }

    // 2. Add host class Rifle
    {
        std::vector<Symbol> docH;
        Symbol hostSym = MakeTestSymbol(SymbolType::Class, "Rifle", "file:///rifle.as");
        hostSym.GetClass().bases.push_back("WeaponMixin");
        docH.push_back(hostSym);
        docH.push_back(MakeTestSymbol(SymbolType::Function, "Fire", "file:///rifle.as", "Rifle"));

        SymbolTable staging;
        for (const auto &sym : docH)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///rifle.as", staging);
    }

    auto inc = table.GetRuleIndex();
    auto orc = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc, *orc);
    CHECK(inc->hostClassesByMixin.contains("WeaponMixin"));
    CHECK(inc->hostClassesByMixin.at("WeaponMixin").size() == 1);
    CHECK(inc->hostClassesByMixin.at("WeaponMixin")[0].name == "Rifle");
}

TEST_CASE("RuleIndex - Equivalence with Overloaded Functions and Shared Names")
{
    SymbolTable table;

    // File A has overloaded global functions and class methods
    {
        std::vector<Symbol> docA;
        docA.push_back(MakeTestSymbol(SymbolType::Function, "Format", "file:///a.as"));
        docA.push_back(MakeTestSymbol(SymbolType::Function, "Format", "file:///a.as"));
        docA.push_back(MakeTestSymbol(SymbolType::Class, "Logger", "file:///a.as"));
        docA.push_back(MakeTestSymbol(SymbolType::Function, "Log", "file:///a.as", "Logger"));
        docA.push_back(MakeTestSymbol(SymbolType::Function, "Log", "file:///a.as", "Logger"));

        SymbolTable staging;
        for (const auto &sym : docA)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///a.as", staging);
    }

    // File B also declares Format (sibling file collision)
    {
        std::vector<Symbol> docB;
        docB.push_back(MakeTestSymbol(SymbolType::Function, "Format", "file:///b.as"));

        SymbolTable staging;
        for (const auto &sym : docB)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///b.as", staging);
    }

    auto inc1 = table.GetRuleIndex();
    auto orc1 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc1, *orc1);
    CHECK(inc1->allNames.at("Format") == 3);
    CHECK(inc1->byContainer.at("Logger").methodNames.size() == 1);
    CHECK(inc1->byContainer.at("Logger").methodCounts.at("Log") == 2);

    // Remove File A
    table.ClearDocumentSymbols("file:///a.as");
    auto inc2 = table.GetRuleIndex();
    auto orc2 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc2, *orc2);
    CHECK(inc2->allNames.at("Format") == 1);
    CHECK_FALSE(inc2->byContainer.contains("Logger"));
}

TEST_CASE("RuleIndex - Reader Snapshot Safety During Mutations")
{
    SymbolTable table;

    // Populate initial state
    {
        std::vector<Symbol> doc;
        doc.push_back(MakeTestSymbol(SymbolType::Class, "Alpha", "file:///alpha.as"));
        doc.push_back(MakeTestSymbol(SymbolType::Function, "Init", "file:///alpha.as", "Alpha"));

        SymbolTable staging;
        for (const auto &sym : doc)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///alpha.as", staging);
    }

    // Reader takes snapshot
    std::shared_ptr<const RuleIndex> readerSnapshot = table.GetRuleIndex();
    CHECK((readerSnapshot.get() != nullptr));
    CHECK(readerSnapshot->allNames.contains("Alpha"));
    CHECK(readerSnapshot->allNames.contains("Init"));
    CHECK_FALSE(readerSnapshot->allNames.contains("Beta"));

    // Mutation occurs on writer thread
    {
        std::vector<Symbol> doc;
        doc.push_back(MakeTestSymbol(SymbolType::Class, "Beta", "file:///beta.as"));
        doc.push_back(MakeTestSymbol(SymbolType::Function, "Run", "file:///beta.as", "Beta"));

        SymbolTable staging;
        for (const auto &sym : doc)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///beta.as", staging);
    }

    // Reader snapshot MUST remain untouched and valid
    CHECK(readerSnapshot->allNames.contains("Alpha"));
    CHECK(readerSnapshot->allNames.contains("Init"));
    CHECK_FALSE(readerSnapshot->allNames.contains("Beta"));

    // New snapshot from table sees the new symbols
    std::shared_ptr<const RuleIndex> newSnapshot = table.GetRuleIndex();
    CHECK((newSnapshot.get() != readerSnapshot.get()));
    CHECK(newSnapshot->allNames.contains("Alpha"));
    CHECK(newSnapshot->allNames.contains("Beta"));
    CHECK(newSnapshot->allNames.contains("Run"));

    // Oracle match on new snapshot
    auto orc = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*newSnapshot, *orc);
}

TEST_CASE("RuleIndex - Equivalence on Mixin Mutation and Multi-File Dependency")
{
    SymbolTable table;

    // File 1: Mixin M1
    {
        std::vector<Symbol> docM;
        Symbol mixinSym = MakeTestSymbol(SymbolType::Class, "M1", "file:///m1.as");
        mixinSym.GetClass().modifiers.isMixin = true;
        docM.push_back(mixinSym);
        docM.push_back(MakeTestSymbol(SymbolType::Function, "M1_Action", "file:///m1.as", "M1"));

        SymbolTable staging;
        for (const auto &sym : docM)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///m1.as", staging);
    }

    // File 2: Host H1 includes M1
    {
        std::vector<Symbol> docH1;
        Symbol host1 = MakeTestSymbol(SymbolType::Class, "H1", "file:///h1.as");
        host1.GetClass().bases.push_back("M1");
        docH1.push_back(host1);
        docH1.push_back(MakeTestSymbol(SymbolType::Function, "H1_Func", "file:///h1.as", "H1"));

        SymbolTable staging;
        for (const auto &sym : docH1)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///h1.as", staging);
    }

    // File 3: Host H2 includes M1 as well
    {
        std::vector<Symbol> docH2;
        Symbol host2 = MakeTestSymbol(SymbolType::Class, "H2", "file:///h2.as");
        host2.GetClass().bases.push_back("M1");
        docH2.push_back(host2);
        docH2.push_back(MakeTestSymbol(SymbolType::Function, "H2_Func", "file:///h2.as", "H2"));

        SymbolTable staging;
        for (const auto &sym : docH2)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///h2.as", staging);
    }

    auto inc1 = table.GetRuleIndex();
    auto orc1 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc1, *orc1);
    CHECK(inc1->hostClassesByMixin.at("M1").size() == 2);

    // Mutate File 1: Add M1_Action2 to Mixin M1
    {
        std::vector<Symbol> docM;
        Symbol mixinSym = MakeTestSymbol(SymbolType::Class, "M1", "file:///m1.as");
        mixinSym.GetClass().modifiers.isMixin = true;
        docM.push_back(mixinSym);
        docM.push_back(MakeTestSymbol(SymbolType::Function, "M1_Action", "file:///m1.as", "M1"));
        docM.push_back(MakeTestSymbol(SymbolType::Function, "M1_Action2", "file:///m1.as", "M1"));

        SymbolTable staging;
        for (const auto &sym : docM)
        {
            staging.AddSymbol(sym);
        }
        table.ReplaceDocumentSymbols("file:///m1.as", staging);
    }

    auto inc2 = table.GetRuleIndex();
    auto orc2 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc2, *orc2);
    CHECK(inc2->byContainer.at("M1").methodNames.contains("M1_Action2"));
    CHECK(inc2->hostClassesByMixin.at("M1").size() == 2);

    // Clear File 1: Mixin M1 is removed
    table.ClearDocumentSymbols("file:///m1.as");
    auto inc3 = table.GetRuleIndex();
    auto orc3 = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc3, *orc3);
    CHECK_FALSE(inc3->hostClassesByMixin.contains("M1"));
    CHECK_FALSE(inc3->byContainer.contains("M1"));
}

TEST_CASE("RuleIndex - Equivalence with AddSymbol and Incremental Maintenance")
{
    SymbolTable table;

    // Add symbols individually via AddSymbol
    table.AddSymbol(MakeTestSymbol(SymbolType::Class, "Widget", "file:///widget.as"));
    table.AddSymbol(MakeTestSymbol(SymbolType::Function, "Paint", "file:///widget.as", "Widget"));
    table.AddSymbol(MakeTestSymbol(SymbolType::Function, "Resize", "file:///widget.as", "Widget"));

    auto inc = table.GetRuleIndex();
    auto orc = RuleIndex::Build(table);
    AssertRuleIndexEquivalence(*inc, *orc);
    CHECK(inc->byContainer.at("Widget").methodNames.size() == 2);
}
