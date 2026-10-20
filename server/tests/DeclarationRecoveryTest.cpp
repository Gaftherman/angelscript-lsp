#include "analysis/DeclarationRecovery.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "document/Document.h"
#include "helpers/LspSemanticHarnessFixture.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "spdlog/fmt/fmt.h"
#include <doctest/doctest.h>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;
using namespace angel_lsp::document;

TEST_SUITE("DeclarationRecovery")
{

    TEST_CASE("Cold Start Swallowed Class Recovery")
    {
        const std::string className = angel_lsp::test::GenerateRandomSymbolName("ASWeaponConfig");
        const std::string cooldownMethod = angel_lsp::test::GenerateRandomSymbolName("GetCooldown");

        // Mimics the user's exact issue: line 3 has 2 quotes ("") instead of 3 (""") closing the heredoc,
        // which previously caused Tree-Sitter to swallow the entire class into a string literal.
        const std::string sourceCode = fmt::format("bool schema = Register(\"Weapon\", \"\"\"{{\n"
                                                   "    \"type\": \"object\"\n"
                                                   "}}\"\" );\n"
                                                   "\n"
                                                   "abstract class {} : IConfigurable\n"
                                                   "{{\n"
                                                   "    float {}(bool trained)\n"
                                                   "    {{\n"
                                                   "        return 0.5f;\n"
                                                   "    }}\n"
                                                   "\n"
                                                   "    const string GetSchema()\n"
                                                   "    {{\n"
                                                   "        return \"\"\"{{\n"
                                                   "            \"test\": 1\n"
                                                   "        }}\"\"\";\n"
                                                   "    }}\n"
                                                   "}}\n",
                                                   className, cooldownMethod);

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///ASWeaponConfig.as";
        auto diags = collector.CollectSymbols(fileUri, sourceCode, parser, table);

        // Document has syntax error due to the broken heredoc delimiter
        CHECK(!diags.empty());

        // The swallowed class MUST be recovered despite the unclosed string before it
        const auto classSymbols = table.FindSymbols(className);
        REQUIRE(!classSymbols.empty());
        CHECK(classSymbols.front().type == SymbolType::Class);
        CHECK(classSymbols.front().startLine >= 4);

        // The class's member method MUST also be recovered with proper offsets
        const auto methodSymbols = table.FindMemberSymbols(className, cooldownMethod);
        REQUIRE(!methodSymbols.empty());
        CHECK(methodSymbols.front().type == SymbolType::Function);
        CHECK(methodSymbols.front().startLine >= 6);
    }

    TEST_CASE("Multi-Declaration Swallowed Recovery (Interface, Enum, Class)")
    {
        const std::string interfaceName = angel_lsp::test::GenerateRandomSymbolName("IWorker");
        const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Status");
        const std::string className = angel_lsp::test::GenerateRandomSymbolName("Manager");
        const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("DoTask");

        // Runaway string literal swallows everything until next quote or recovery
        const std::string sourceCode = fmt::format("string s = \"unclosed string;\n"
                                                   "\n"
                                                   "interface {}\n"
                                                   "{{\n"
                                                   "    void {}();\n"
                                                   "}}\n"
                                                   "\n"
                                                   "enum {}\n"
                                                   "{{\n"
                                                   "    VAL_A,\n"
                                                   "    VAL_B\n"
                                                   "}}\n"
                                                   "\n"
                                                   "class {}\n"
                                                   "{{\n"
                                                   "    {}@ m_worker;\n"
                                                   "}}\n",
                                                   interfaceName, methodName, enumName, className, interfaceName);

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///MultiSwallowed.as";
        collector.CollectSymbols(fileUri, sourceCode, parser, table);

        // All three declarations must be recovered
        const auto ifaceSyms = table.FindSymbols(interfaceName);
        REQUIRE(!ifaceSyms.empty());
        CHECK(ifaceSyms.front().type == SymbolType::Interface);

        const auto enumSyms = table.FindSymbols(enumName);
        REQUIRE(!enumSyms.empty());
        CHECK(enumSyms.front().type == SymbolType::Enum);

        const auto classSyms = table.FindSymbols(className);
        REQUIRE(!classSyms.empty());
        CHECK(classSyms.front().type == SymbolType::Class);
    }

    TEST_CASE("Last-Known-Good Retention on Incremental Edit")
    {
        const std::string className = angel_lsp::test::GenerateRandomSymbolName("ConfigClass");
        const std::string methodName = angel_lsp::test::GenerateRandomSymbolName("ExecuteAction");
        const std::string fileUri = "file:///LiveEditing.as";

        // 1. Initial clean version
        const std::string cleanSource = fmt::format("class {}\n"
                                                    "{{\n"
                                                    "    void {}() {{}}\n"
                                                    "}}\n",
                                                    className, methodName);

        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        SymbolTable activeTable;

        collector.CollectSymbols(fileUri, cleanSource, parser, activeTable);
        REQUIRE(!activeTable.FindSymbols(className).empty());
        REQUIRE(!activeTable.FindMemberSymbols(className, methodName).empty());

        // 2. Incremental edit with syntax error (user typed a broken quote)
        const std::string brokenSource = fmt::format("string broken = \"unclosed;\n"
                                                     "class {}\n"
                                                     "{{\n"
                                                     "    void {}() {{}}\n"
                                                     "}}\n",
                                                     className, methodName);

        SymbolTable stagingTable;
        angel_lsp::document::TreePtr brokenTree = angel_lsp::document::MakeTreePtr(parser.Parse(brokenSource));
        REQUIRE(brokenTree != nullptr);
        CHECK(ts_node_has_error(ts_tree_root_node(brokenTree.get())));

        collector.CollectSymbolsWithTree({fileUri, brokenSource}, brokenTree.get(), stagingTable);

        // Staging table might have recovered or lost it; RetainLastKnownGoodTypes guarantees retention
        activeTable.RetainLastKnownGoodTypes(fileUri, stagingTable);

        CHECK(!stagingTable.FindSymbols(className).empty());
        CHECK(!stagingTable.FindMemberSymbols(className, methodName).empty());

        // Replace table with staging - active table retains Last Known Good!
        activeTable.ReplaceDocumentSymbols(fileUri, std::move(stagingTable));
        CHECK(!activeTable.FindSymbols(className).empty());
        CHECK(!activeTable.FindMemberSymbols(className, methodName).empty());
    }

    TEST_CASE("Cross-Document Resolution With Swallowed Class Produces Zero Errors on Dependent File")
    {
        const std::string configClass = angel_lsp::test::GenerateRandomSymbolName("WeaponConfig");
        const std::string cooldownFn = angel_lsp::test::GenerateRandomSymbolName("GetCooldown");

        angel_lsp::test::LspSemanticHarnessFixture fixture;

        const std::string configUri = fixture.SandboxUri("Config.as");
        const std::string weaponUri = fixture.SandboxUri("Weapon.as");

        // Config.as has the exact 2-quotes typo in heredoc, followed by the class
        const std::string configContent = fmt::format("bool schema = Register(\"Weapon\", \"\"\"{{\n"
                                                      "    \"type\": \"object\"\n"
                                                      "}}\"\" );\n"
                                                      "\n"
                                                      "abstract class {}\n"
                                                      "{{\n"
                                                      "    float {}(bool trained)\n"
                                                      "    {{\n"
                                                      "        return 0.2f;\n"
                                                      "    }}\n"
                                                      "}}\n",
                                                      configClass, cooldownFn);

        // Weapon.as includes Config.as and calls the method
        const std::string weaponContent = fmt::format("#include \"Config.as\"\n"
                                                      "\n"
                                                      "void FireWeapon({}@ cfg)\n"
                                                      "{{\n"
                                                      "    float cd = cfg.{}(true);\n"
                                                      "    if (cd > 0.0f) {{}}\n"
                                                      "}}\n",
                                                      configClass, cooldownFn);

        fixture.AddVirtualDocument(configUri, configContent);
        fixture.AddVirtualDocument(weaponUri, weaponContent);

        // Config.as has its syntax error diagnostic
        const auto& configDiags = fixture.GetDiagnostics(configUri);
        CHECK(!configDiags.empty());

        // CRITICAL INVARIANT: Weapon.as MUST NOT have any errors!
        // Zero E103 (Unknown type), Zero E115 (No member on class), Zero cascading errors!
        fixture.AssertNoDiagnostics(weaponUri);
    }

} // TEST_SUITE("DeclarationRecovery")
