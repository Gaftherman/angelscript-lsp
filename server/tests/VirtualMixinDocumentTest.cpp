#include <doctest/doctest.h>

#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/ScopeTree.h"
#include "features/definition/DefinitionHandler.h"
#include "features/hover/HoverHandler.h"
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

    SUBCASE("Feature flag enabled: Go-to-Definition returns physical mixin URI and line")
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

        CHECK((*defs)[0].uri.toString() == mixinUri);
        CHECK((*defs)[0].range.start.line == 1);

        ts_tree_delete(tree);
    }
}

TEST_CASE("VirtualMixinDocument - Host-Scope Fallback for Hover and Definition")
{
    AngelScriptParser parser;
    SymbolCollector collector{ nullptr };
    LocalScopeCollector scopeCollector{ nullptr };

    std::string mixinUri = "file:///mixin.as";
    std::string mixinCode =
        "mixin class WeaponMixin {\n"
        "    void Attack() {\n"
        "        self.FireWeapon();\n"
        "        m_flNextAttack = 1.0f;\n"
        "    }\n"
        "}\n";

    std::string hostUri = "file:///host.as";
    std::string hostCode =
        "class CBasePlayerWeapon {\n"
        "    float m_flNextAttack;\n"
        "    void FireWeapon() {}\n"
        "}\n"
        "class MyRifle : CBasePlayerWeapon, WeaponMixin {\n"
        "    CBasePlayerWeapon@ self;\n"
        "}\n";

    SymbolTable table;
    ScopeIndex scopeIndex;
    table.SetVirtualMixinDocumentsEnabled(true);

    collector.CollectSymbols(mixinUri, mixinCode, parser, table);
    collector.CollectSymbols(hostUri, hostCode, parser, table);
    table.ResolveIncludedMixins();

    std::string virtualUri = "angelscript-virtual://MyRifle/WeaponMixin.as";
    TSTree *tree = parser.Parse(mixinCode);
    REQUIRE(tree != nullptr);

    auto rootScope = scopeCollector.CollectScopes(mixinCode, parser);
    if (rootScope)
    {
        scopeIndex.SetScopeTree(virtualUri, std::move(rootScope));
    }

    SUBCASE("Hover on 'self' in virtual document resolves host property signature")
    {
        // Cursor on "self" at line 2, character 9
        features::HoverRequest req{ virtualUri, mixinCode, tree, table, scopeIndex, lsp::Position{ 2, 9 } };
        auto hover = features::GetHover(req);
        REQUIRE(hover.has_value());
        const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(markup.value.find("self") != std::string::npos);
        CHECK(markup.value.find("CBasePlayerWeapon") != std::string::npos);
    }

    SUBCASE("Hover on host member 'FireWeapon' in virtual document resolves host method signature")
    {
        // Cursor on "FireWeapon" at line 2, character 15
        features::HoverRequest req{ virtualUri, mixinCode, tree, table, scopeIndex, lsp::Position{ 2, 15 } };
        auto hover = features::GetHover(req);
        REQUIRE(hover.has_value());
        const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(markup.value.find("FireWeapon") != std::string::npos);
    }

    SUBCASE("Hover on host inherited property 'm_flNextAttack' resolves float type")
    {
        // Cursor on "m_flNextAttack" at line 3, character 10
        features::HoverRequest req{ virtualUri, mixinCode, tree, table, scopeIndex, lsp::Position{ 3, 10 } };
        auto hover = features::GetHover(req);
        REQUIRE(hover.has_value());
        const auto &markup = std::get<lsp::MarkupContent>(hover->contents);
        CHECK(markup.value.find("m_flNextAttack") != std::string::npos);
        CHECK(markup.value.find("float") != std::string::npos);
    }

    SUBCASE("Go-to-Definition on 'self' in virtual document navigates to host declaration")
    {
        // Cursor on "self" at line 2, character 9
        features::DefinitionRequest req{ virtualUri, mixinCode, tree, table, scopeIndex, lsp::Position{ 2, 9 } };
        auto defs = features::GetDefinition(req);
        REQUIRE(defs.has_value());
        REQUIRE(!defs->empty());
        CHECK((*defs)[0].uri.toString() == hostUri);
        CHECK((*defs)[0].range.start.line == 5);
    }

    SUBCASE("Go-to-Definition on host member 'FireWeapon' in virtual document navigates to host declaration")
    {
        // Cursor on "FireWeapon" at line 2, character 15
        features::DefinitionRequest req{ virtualUri, mixinCode, tree, table, scopeIndex, lsp::Position{ 2, 15 } };
        auto defs = features::GetDefinition(req);
        REQUIRE(defs.has_value());
        REQUIRE(!defs->empty());
        CHECK((*defs)[0].uri.toString() == hostUri);
        CHECK((*defs)[0].range.start.line == 2);
    }

    ts_tree_delete(tree);
}


