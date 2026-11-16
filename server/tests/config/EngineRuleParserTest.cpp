#include "config/EngineRuleParser.h"

#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <filesystem>
#include <random>
#include <string>

TEST_SUITE("config/EngineRuleParser")
{
    TEST_CASE("EngineRuleParser - Parses valid noHandleTypes array")
    {
        std::mt19937_64 rng(0x9011A4D1);
        const std::string typeA = angel_lsp::test::GenerateIdentifier(rng, "ValueType");
        const std::string typeB = angel_lsp::test::GenerateIdentifier(rng, "OpaqueVal");

        const std::string jsonText = R"({"name": "Custom", "noHandleTypes": [")" + typeA + R"(", ")" + typeB + R"("]})";

        const auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(jsonText);
        REQUIRE(parsed.has_value());
        CHECK(parsed->HasRules());
        CHECK(parsed->noHandleTypes.size() == 2);
        CHECK(parsed->noHandleTypes.contains(typeA));
        CHECK(parsed->noHandleTypes.contains(typeB));
    }

    TEST_CASE("EngineRuleParser - Absent noHandleTypes leaves set empty")
    {
        const auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(R"({"name": "Vanilla"})");
        REQUIRE(parsed.has_value());
        CHECK(parsed->noHandleTypes.empty());
        CHECK_FALSE(parsed->HasRules());
    }

    TEST_CASE("EngineRuleParser - Rejects malformed noHandleTypes values")
    {
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": "string"})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": 42})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": null})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": {}})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": [123]})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": ["ok", false]})").has_value());
        CHECK_FALSE(angel_lsp::config::ParseEngineRuleConfigJson(R"({"noHandleTypes": [""]})").has_value());
    }

    TEST_CASE("EngineRuleParser - Loads sdk-addons fixture rules")
    {
        const std::filesystem::path fixturePath =
            std::filesystem::path(ANGELSCRIPT_FIXTURE_DIR) / "sdk-addons.angelscript.rules.json";
        const auto parsed = angel_lsp::config::ParseEngineRuleConfigFile(fixturePath);
        REQUIRE(parsed.has_value());
        CHECK(parsed->noHandleTypes.size() == 3);
        CHECK(parsed->noHandleTypes.contains("string"));
        CHECK(parsed->noHandleTypes.contains("datetime"));
        CHECK(parsed->noHandleTypes.contains("complex"));
    }
}
