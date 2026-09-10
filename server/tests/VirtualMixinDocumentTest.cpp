#include <doctest/doctest.h>

#include "analysis/SymbolTable.h"
#include "analysis/SymbolCollector.h"
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
