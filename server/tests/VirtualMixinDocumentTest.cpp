#include <doctest/doctest.h>

#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "features/definition/DefinitionHandler.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("VirtualMixinDocument - URI Building and Formatting")
{
    CHECK(SymbolTable::BuildVirtualMixinUri("CWeaponIns2Garand", "CASWeaponMixin") ==
          "angelscript-virtual://CWeaponIns2Garand/CASWeaponMixin.as");
    CHECK(SymbolTable::BuildVirtualMixinUri("CWeaponIns2Garand", "CASWeaponMixin.as") ==
          "angelscript-virtual://CWeaponIns2Garand/CASWeaponMixin.as");
}

TEST_CASE("VirtualMixinDocument - Toggle Feature Flag and Synthetic URIs")
{
    AngelScriptParser parser;
    SymbolCollector collector{ nullptr };

    std::string mixinUri = "file:///mixin.as";
    std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Deploy(int speed) {}\n"
        "}\n";

    std::string hostUri = "file:///host.as";
    std::string hostCode =
        "class Rifle : WeaponMixin {\n"
        "}\n";

    SUBCASE("Disabled by default: virtualFileUri is empty")
    {
        SymbolTable table;
        CHECK_FALSE(table.IsVirtualMixinDocumentsEnabled());

        collector.CollectSymbols(mixinUri, mixinCode, parser, table);
        collector.CollectSymbols(hostUri, hostCode, parser, table);
        table.ResolveIncludedMixins();

        auto symbols = table.FindSymbols("Rifle::Deploy");
        REQUIRE(!symbols.empty());
        CHECK(symbols[0].isSynthesized);
        CHECK(symbols[0].fileUri == mixinUri);
        CHECK(symbols[0].virtualFileUri.empty());
    }

    SUBCASE("Enabled: virtualFileUri is populated with synthetic URI")
    {
        SymbolTable table;
        table.SetVirtualMixinDocumentsEnabled(true);
        CHECK(table.IsVirtualMixinDocumentsEnabled());

        collector.CollectSymbols(mixinUri, mixinCode, parser, table);
        collector.CollectSymbols(hostUri, hostCode, parser, table);
        table.ResolveIncludedMixins();

        auto symbols = table.FindSymbols("Rifle::Deploy");
        REQUIRE(!symbols.empty());
        CHECK(symbols[0].isSynthesized);
        CHECK(symbols[0].fileUri == mixinUri);
        CHECK(symbols[0].virtualFileUri == "angelscript-virtual://Rifle/WeaponMixin.as");
    }
}

TEST_CASE("VirtualMixinDocument - Definition Routing Physical vs Virtual")
{
    AngelScriptParser parser;
    SymbolCollector collector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };

    std::string mixinUri = "file:///mixin.as";
    std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Deploy(int speed) {}\n"
        "}\n";

    std::string callerUri = "file:///caller.as";
    std::string callerCode =
        "class Rifle : WeaponMixin {}\n"
        "void Main() {\n"
        "    Rifle r;\n"
        "    r.Deploy(42);\n"
        "}\n";

    SUBCASE("Feature flag disabled: Go-to-Definition returns physical mixin URI and line")
    {
        SymbolTable table;
        ScopeIndex scopeIndex;
        table.SetVirtualMixinDocumentsEnabled(false);

        collector.CollectSymbols(mixinUri, mixinCode, parser, table);
        collector.CollectSymbols(callerUri, callerCode, parser, table);
        table.ResolveIncludedMixins();

        auto rootScope = scopeCollector.CollectScopes(callerCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(callerUri, std::move(rootScope));
        }

        TSTree *tree = parser.Parse(callerCode);
        REQUIRE(tree != nullptr);

        // Cursor on "Deploy" at line 3, character 6
        features::DefinitionRequest req{ callerUri, callerCode, tree, table, scopeIndex, lsp::Position{ 3, 6 } };
        auto defs = features::GetDefinition(req);
        REQUIRE(defs.has_value());
        REQUIRE(!defs->empty());

        CHECK((*defs)[0].uri.toString() == mixinUri);
        CHECK((*defs)[0].range.start.line == 1);

        ts_tree_delete(tree);
    }

    SUBCASE("Feature flag enabled: Go-to-Definition returns virtual mixin URI with mapped line offset")
    {
        SymbolTable table;
        ScopeIndex scopeIndex;
        table.SetVirtualMixinDocumentsEnabled(true);

        collector.CollectSymbols(mixinUri, mixinCode, parser, table);
        collector.CollectSymbols(callerUri, callerCode, parser, table);
        table.ResolveIncludedMixins();

        auto rootScope = scopeCollector.CollectScopes(callerCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(callerUri, std::move(rootScope));
        }

        TSTree *tree = parser.Parse(callerCode);
        REQUIRE(tree != nullptr);

        // Cursor on "Deploy" at line 3, character 6
        features::DefinitionRequest req{ callerUri, callerCode, tree, table, scopeIndex, lsp::Position{ 3, 6 } };
        auto defs = features::GetDefinition(req);
        REQUIRE(defs.has_value());
        REQUIRE(!defs->empty());

        CHECK((*defs)[0].uri.toString() == "angelscript-virtual://Rifle/WeaponMixin.as");
        // Header is 3 lines (0, 1, 2). WeaponMixin starts at line 0 in mixin.as.
        // Deploy is at line 1 in mixin.as.
        // Mapped line = 3 + (1 - 0) = 4.
        CHECK((*defs)[0].range.start.line == 4);

        ts_tree_delete(tree);
    }
}

