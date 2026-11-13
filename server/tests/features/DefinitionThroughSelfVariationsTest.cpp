#include "analysis/SemanticAnalyzer.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "config/EngineRuleConfig.h"
#include "features/common/FeatureSharedUtils.h"
#include "features/definition/DefinitionHandler.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

using namespace angel_lsp;
using namespace angel_lsp::features;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
struct SourcePos
{
    uint32_t line = 0;
    uint32_t character = 0;
};

SourcePos FindPos(const std::string& source, const std::string& needle, size_t startAt = 0)
{
    size_t pos = source.find(needle, startAt);
    if (pos == std::string::npos)
        return {0, 0};
    uint32_t line = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < pos; ++i)
    {
        if (source[i] == '\n')
        {
            ++line;
            lineStart = i + 1;
        }
    }
    return {line, static_cast<uint32_t>(pos - lineStart)};
}

struct TestEnvironment
{
    AngelScriptParser parser;
    SymbolCollector symbolCollector{nullptr};
    LocalScopeCollector scopeCollector{nullptr};
    SymbolTable symbolTable;
    ScopeIndex scopeIndex;
    std::string uri = "file:///test.as";
    std::string sourceCode;
    TSTree* tree = nullptr;

    TestEnvironment(const std::string& code, const std::string& docUri = "file:///test.as")
        : uri(docUri), sourceCode(code)
    {
        tree = parser.Parse(sourceCode);
        symbolCollector.CollectSymbols(uri, sourceCode, parser, symbolTable);
        auto rootScope = scopeCollector.CollectScopes(sourceCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(uri, std::move(rootScope));
        }
    }

    void AddFile(const std::string& otherUri, const std::string& otherCode)
    {
        symbolCollector.CollectSymbols(otherUri, otherCode, parser, symbolTable);
        auto rootScope = scopeCollector.CollectScopes(otherCode, parser);
        if (rootScope)
        {
            scopeIndex.SetScopeTree(otherUri, std::move(rootScope));
        }
    }

    ~TestEnvironment()
    {
        if (tree)
        {
            ts_tree_delete(tree);
        }
    }

    std::function<std::string(const std::string&)> resolveInclude = {};
    config::EngineRuleConfig engineRules;

    std::optional<std::vector<lsp::Location>> DefAt(uint32_t line, uint32_t character)
    {
        symbolTable.SetBaseClassDelegateProperties(engineRules.unusedRules.baseClassDelegateProperties);
        DefinitionRequest req{
            .query = {uri, sourceCode, symbolTable, scopeIndex, line, character, ".as.predefined", &engineRules},
            .resolveInclude = [this](std::string_view rawPath) -> std::optional<lsp::DocumentUri>
            {
                if (!resolveInclude)
                {
                    return std::nullopt;
                }
                std::string resolved = resolveInclude(std::string(rawPath));
                if (resolved.empty())
                {
                    return std::nullopt;
                }
                return features::common::ParseOrConvertUri(resolved);
            },
        };
        return GetDefinition(req);
    }
};
} // namespace

TEST_SUITE("DefinitionThroughSelfVariations")
{
    TEST_CASE("Definition - Base class property reached through this.")
    {
        const std::string baseClass = angel_lsp::test::GenerateRandomSymbolName("BaseEntity");
        const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("PlayerEntity");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("ammoCount");
        const std::string userUri = "file:///scripts/main.as";
        const std::string code = "class " + baseClass +
                                 " {\n"
                                 "    int " +
                                 propName +
                                 ";\n"
                                 "}\n"
                                 "class " +
                                 derivedClass + " : " + baseClass +
                                 " {\n"
                                 "    void Reload() {\n"
                                 "        this." +
                                 propName +
                                 " = 30;\n"
                                 "    }\n"
                                 "}\n";

        TestEnvironment env(code, userUri);
        const auto declPos = FindPos(code, propName);
        const size_t thisPos = code.find("this." + propName) + 5;
        const auto usePos = FindPos(code, propName, thisPos);

        auto def = env.DefAt(usePos.line, usePos.character);
        REQUIRE(def.has_value());
        REQUIRE(!def->empty());
        CHECK((*def)[0].uri.toString() == userUri);
        CHECK((*def)[0].range.start.line == declPos.line);
    }

    TEST_CASE("Definition - Property declared in a base two levels up reached through this.")
    {
        const std::string grandBase = angel_lsp::test::GenerateRandomSymbolName("GrandEntity");
        const std::string midBase = angel_lsp::test::GenerateRandomSymbolName("MidActor");
        const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("HeroCharacter");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("health");
        const std::string userUri = "file:///scripts/player.as";
        const std::string code = "class " + grandBase +
                                 " {\n"
                                 "    float " +
                                 propName +
                                 ";\n"
                                 "}\n"
                                 "class " +
                                 midBase + " : " + grandBase +
                                 " {\n"
                                 "}\n"
                                 "class " +
                                 derivedClass + " : " + midBase +
                                 " {\n"
                                 "    void Heal() {\n"
                                 "        this." +
                                 propName +
                                 " = 100.0f;\n"
                                 "    }\n"
                                 "}\n";

        TestEnvironment env(code, userUri);
        const auto declPos = FindPos(code, propName);
        const size_t thisPos = code.find("this." + propName) + 5;
        const auto usePos = FindPos(code, propName, thisPos);

        auto def = env.DefAt(usePos.line, usePos.character);
        REQUIRE(def.has_value());
        REQUIRE(!def->empty());
        CHECK((*def)[0].uri.toString() == userUri);
        CHECK((*def)[0].range.start.line == declPos.line);
    }

    TEST_CASE("Definition - Virtual property accessor reached through this.")
    {
        const std::string baseClass = angel_lsp::test::GenerateRandomSymbolName("VehicleBase");
        const std::string derivedClass = angel_lsp::test::GenerateRandomSymbolName("CarModel");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("speed");
        const std::string userUri = "file:///scripts/vehicle.as";
        const std::string code = "class " + baseClass +
                                 " {\n"
                                 "    int " +
                                 propName +
                                 " {\n"
                                 "        get const { return 0; }\n"
                                 "        set {}\n"
                                 "    }\n"
                                 "}\n"
                                 "class " +
                                 derivedClass + " : " + baseClass +
                                 " {\n"
                                 "    void Accelerate() {\n"
                                 "        this." +
                                 propName +
                                 " = 60;\n"
                                 "    }\n"
                                 "}\n";

        TestEnvironment env(code, userUri);
        const auto declPos = FindPos(code, propName);
        const size_t thisPos = code.find("this." + propName) + 5;
        const auto usePos = FindPos(code, propName, thisPos);

        auto def = env.DefAt(usePos.line, usePos.character);
        REQUIRE(def.has_value());
        REQUIRE(!def->empty());
        CHECK((*def)[0].uri.toString() == userUri);
        CHECK((*def)[0].range.start.line == declPos.line);
    }

    TEST_CASE("Definition - Chained assignment members reached through self.")
    {
        const std::string entity = angel_lsp::test::GenerateRandomSymbolName("WeaponData");
        const std::string base = angel_lsp::test::GenerateRandomSymbolName("WeaponScriptBase");
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Pistol");
        const std::string propA = angel_lsp::test::GenerateRandomSymbolName("primaryCooldown");
        const std::string propB = angel_lsp::test::GenerateRandomSymbolName("secondaryCooldown");
        const std::string predefUri = "file:///predefined/weapon.as.predefined";
        const std::string predefCode = "class " + entity +
                                       " {\n"
                                       "    float " +
                                       propA +
                                       ";\n"
                                       "    float " +
                                       propB +
                                       ";\n"
                                       "}\n"
                                       "abstract class " +
                                       base +
                                       " {\n"
                                       "    private " +
                                       entity +
                                       "@ m_data;\n"
                                       "    " +
                                       entity +
                                       "@ self { get const { return @m_data; } }\n"
                                       "}\n";
        const std::string userUri = "file:///scripts/pistol.as";
        const std::string userCode = "class " + cls + " : " + base +
                                     " {\n"
                                     "    void ResetCooldowns() {\n"
                                     "        self." +
                                     propA + " = self." + propB +
                                     " = 0.5f;\n"
                                     "    }\n"
                                     "}\n";

        TestEnvironment env(userCode, userUri);
        env.AddFile(predefUri, predefCode);

        const auto declPosA = FindPos(predefCode, propA);
        const auto declPosB = FindPos(predefCode, propB);

        const size_t posAInChain = userCode.find("self." + propA) + 5;
        const auto usePosA = FindPos(userCode, propA, posAInChain);
        auto defA = env.DefAt(usePosA.line, usePosA.character);
        REQUIRE(defA.has_value());
        REQUIRE(!defA->empty());
        CHECK((*defA)[0].uri.toString() == predefUri);
        CHECK((*defA)[0].range.start.line == declPosA.line);

        const size_t posBInChain = userCode.find("self." + propB) + 5;
        const auto usePosB = FindPos(userCode, propB, posBInChain);
        auto defB = env.DefAt(usePosB.line, usePosB.character);
        REQUIRE(defB.has_value());
        REQUIRE(!defB->empty());
        CHECK((*defB)[0].uri.toString() == predefUri);
        CHECK((*defB)[0].range.start.line == declPosB.line);
    }

    TEST_CASE("Definition - Member inside a namespaced class reached through self.")
    {
        const std::string entity = angel_lsp::test::GenerateRandomSymbolName("EngineEntity");
        const std::string base = angel_lsp::test::GenerateRandomSymbolName("ScriptBase");
        const std::string ns = angel_lsp::test::GenerateRandomSymbolName("GameNs");
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Monster");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("entityId");
        const std::string predefUri = "file:///predefined/engine.as.predefined";
        const std::string predefCode = "class " + entity +
                                       " {\n"
                                       "    int " +
                                       propName +
                                       ";\n"
                                       "}\n"
                                       "abstract class " +
                                       base +
                                       " {\n"
                                       "    private " +
                                       entity +
                                       "@ m_entity;\n"
                                       "    " +
                                       entity +
                                       "@ self { get const { return @m_entity; } }\n"
                                       "}\n";
        const std::string userUri = "file:///scripts/monster.as";
        const std::string userCode = "namespace " + ns +
                                     " {\n"
                                     "class " +
                                     cls + " : " + base +
                                     " {\n"
                                     "    void Setup() {\n"
                                     "        self." +
                                     propName +
                                     " = 42;\n"
                                     "    }\n"
                                     "}\n"
                                     "}\n";

        TestEnvironment env(userCode, userUri);
        env.AddFile(predefUri, predefCode);

        const auto declPos = FindPos(predefCode, propName);
        const size_t thisPos = userCode.find("self." + propName) + 5;
        const auto usePos = FindPos(userCode, propName, thisPos);

        auto def = env.DefAt(usePos.line, usePos.character);
        REQUIRE(def.has_value());
        REQUIRE(!def->empty());
        CHECK((*def)[0].uri.toString() == predefUri);
        CHECK((*def)[0].range.start.line == declPos.line);
    }

    TEST_CASE("Definition - Member after a method call in a chain reached through self.")
    {
        const std::string targetCls = angel_lsp::test::GenerateRandomSymbolName("TargetNode");
        const std::string radarCls = angel_lsp::test::GenerateRandomSymbolName("Radar");
        const std::string base = angel_lsp::test::GenerateRandomSymbolName("SensorBase");
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Tracker");
        const std::string method = angel_lsp::test::GenerateRandomSymbolName("GetCurrentTarget");
        const std::string propName = angel_lsp::test::GenerateRandomSymbolName("priority");
        const std::string predefUri = "file:///predefined/sensor.as.predefined";
        const std::string predefCode = "class " + targetCls +
                                       " {\n"
                                       "    int " +
                                       propName +
                                       ";\n"
                                       "}\n"
                                       "class " +
                                       radarCls +
                                       " {\n"
                                       "    " +
                                       targetCls + "@ " + method +
                                       "();\n"
                                       "}\n"
                                       "abstract class " +
                                       base +
                                       " {\n"
                                       "    private " +
                                       radarCls +
                                       "@ m_radar;\n"
                                       "    " +
                                       radarCls +
                                       "@ self { get const { return @m_radar; } }\n"
                                       "}\n";
        const std::string userUri = "file:///scripts/tracker.as";
        const std::string userCode = "class " + cls + " : " + base +
                                     " {\n"
                                     "    void Update() {\n"
                                     "        self." +
                                     method + "()." + propName +
                                     " = 10;\n"
                                     "    }\n"
                                     "}\n";

        TestEnvironment env(userCode, userUri);
        env.AddFile(predefUri, predefCode);

        const auto declPos = FindPos(predefCode, propName);
        const size_t propInChain = userCode.find("()." + propName) + 3;
        const auto usePos = FindPos(userCode, propName, propInChain);

        auto def = env.DefAt(usePos.line, usePos.character);
        REQUIRE(def.has_value());
        REQUIRE(!def->empty());
        CHECK((*def)[0].uri.toString() == predefUri);
        CHECK((*def)[0].range.start.line == declPos.line);
    }
}
