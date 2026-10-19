#include <doctest/doctest.h>

#include "analysis/EngineConfigConverter.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

using namespace angel_lsp::analysis;

TEST_CASE("EngineConfigConverter - Detection recognizes official config dumps")
{
    const std::string officialDump = "// AngelScript 2.38.0\n"
                                     "// Lib options \n"
                                     "ep 0 0\n"
                                     "ep 14 2\n"
                                     "objtype \"Vector3\" 2\n";

    CHECK(IsOfficialEngineConfig(officialDump));

    const std::string regularCode = "void main()\n"
                                    "{\n"
                                    "    int a = 10;\n"
                                    "}\n";

    CHECK_FALSE(IsOfficialEngineConfig(regularCode));
    CHECK_FALSE(IsOfficialEngineConfig(""));
}

TEST_CASE("EngineConfigConverter - Converts enums, classes, methods, and ep dialect properties")
{
    const std::string enumName = angel_lsp::test::GenerateRandomSymbolName("Color");
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Vector3");
    const std::string fnName = angel_lsp::test::GenerateRandomSymbolName("PrintMessage");

    const std::string configDump = "// AngelScript 2.38.0\n"
                                   "ep 14 2\n"
                                   "ep 20 1\n"
                                   "enum " +
                                   enumName +
                                   " int\n"
                                   "enumval " +
                                   enumName +
                                   " Red 0\n"
                                   "enumval " +
                                   enumName +
                                   " Blue 1\n"
                                   "objtype \"" +
                                   clsName +
                                   "\" 2\n"
                                   "objbeh \"" +
                                   clsName + "\" 0 \"void " + clsName +
                                   "(float x, float y)\"\n"
                                   "objbeh \"" +
                                   clsName + "\" 1 \"void ~" + clsName +
                                   "()\"\n"
                                   "objprop \"" +
                                   clsName +
                                   "\" \"float x\" 0 0\n"
                                   "objmthd \"" +
                                   clsName +
                                   "\" \"float Length() const\"\n"
                                   "func \"void " +
                                   fnName +
                                   "(const string &in text)\"\n"
                                   "prop \"const int MAX_PLAYERS\"\n";

    const auto result = ConvertEngineConfigToPredefined(configDump);

    // Assert engine properties extracted
    CHECK(result.engineProperties.propertyAccessorMode == 2);
    CHECK(result.engineProperties.disallowValueAssignForRef == true);

    // Assert content elements
    const std::string& predef = result.predefinedContent;
    CHECK(predef.find("enum " + enumName) != std::string::npos);
    CHECK(predef.find("Red = 0,") != std::string::npos);
    CHECK(predef.find("Blue = 1,") != std::string::npos);
    CHECK(predef.find("class " + clsName) != std::string::npos);
    CHECK(predef.find(clsName + "(float x, float y);") != std::string::npos);
    CHECK(predef.find("~" + clsName + "();") != std::string::npos);
    CHECK(predef.find("float x;") != std::string::npos);
    CHECK(predef.find("float Length() const;") != std::string::npos);
    CHECK(predef.find("void " + fnName + "(const string &in text);") != std::string::npos);
    CHECK(predef.find("const int MAX_PLAYERS;") != std::string::npos);

    // Verify it parses cleanly into SymbolTable
    angel_lsp::parser::AngelScriptParser parser;
    SymbolTable table;
    angel_lsp::i18n::I18n i18n;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols("file:///as_config.as.predefined", predef, parser, table);

    CHECK(table.FindSymbolsPtr(clsName).get() != nullptr);
    CHECK(table.FindSymbolsPtr(enumName).get() != nullptr);
    CHECK(table.FindSymbolsPtr(fnName).get() != nullptr);
}

TEST_CASE("EngineConfigConverter - Handles scoped namespaces cleanly")
{
    const std::string nsName = angel_lsp::test::GenerateRandomSymbolName("Game");
    const std::string clsName = angel_lsp::test::GenerateRandomSymbolName("Player");

    const std::string configDump = "namespace \"" + nsName +
                                   "\"\n"
                                   "objtype \"" +
                                   clsName +
                                   "\" 4\n"
                                   "objprop \"" +
                                   clsName +
                                   "\" \"int m_health\" 0 0\n"
                                   "namespace \"\"\n"
                                   "func \"void GlobalInit()\"\n";

    const auto result = ConvertEngineConfigToPredefined(configDump);
    const std::string& predef = result.predefinedContent;

    CHECK(predef.find("namespace " + nsName) != std::string::npos);
    CHECK(predef.find("class " + clsName) != std::string::npos);
    CHECK(predef.find("int m_health;") != std::string::npos);
    CHECK(predef.find("void GlobalInit();") != std::string::npos);
}
