#include "config/EngineVersionFeatures.h"

#include "config/EngineRuleParser.h"

#include <doctest/doctest.h>
#include <random>
#include <string>

TEST_SUITE("config/EngineVersionFeatures")
{
    TEST_CASE("EngineVersionFeatures - Converts versions to the engine number")
    {
        std::mt19937_64 rng(0x7E1C0D3);
        for (int round = 0; round < 50; ++round)
        {
            const int major = static_cast<int>(rng() % 10);
            const int minor = static_cast<int>(rng() % 100);
            const int patch = static_cast<int>(rng() % 100);
            const std::string text = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
            const int expected = major * 10000 + minor * 100 + patch;
            if (expected == 0)
            {
                continue;
            }
            CHECK(angel_lsp::config::EngineVersionNumber(text) == expected);
            CHECK(angel_lsp::config::IsEngineVersionString(text));
        }
        CHECK(angel_lsp::config::EngineVersionNumber("2.39.0") == 23900);
        for (const char* bad : {"", "2", "2.39", "2.39.0.1", "2..0", "a.b.c", "2.39.x", " 2.39.0", "2.39.0 ", "-2.1.1"})
        {
            CHECK_FALSE(angel_lsp::config::IsEngineVersionString(bad));
        }
    }

    TEST_CASE("EngineVersionFeatures - Table is sorted by key and every introduction is a valid version")
    {
        std::string previous;
        for (const auto& entry : angel_lsp::config::EnginePropertyIntroductions())
        {
            CHECK(std::string(entry.key) > previous);
            previous = std::string(entry.key);
            CHECK(angel_lsp::config::IsEngineVersionString(entry.sinceVersion));
            CHECK(angel_lsp::config::EngineVersionNumber(entry.sinceVersion) <=
                  angel_lsp::config::EngineVersionNumber(angel_lsp::config::LATEST_SUPPORTED_ENGINE_VERSION));
        }
    }

    TEST_CASE("EngineVersionFeatures - Reports properties newer than the declared version")
    {
        const auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(
            R"({"engine": {"name": "Host", "version": "2.36.1"}, "engineProperties": {"foreachSupport": true, "requireEnumScope": true}})");
        REQUIRE(parsed.has_value());
        CHECK(parsed->profile.declared);
        CHECK(parsed->profile.version == "2.36.1");
        CHECK(parsed->name == "Host");
        const auto issues = angel_lsp::config::CheckEngineProfile(*parsed);
        REQUIRE(issues.size() == 1);
        CHECK(issues[0].key == "foreachSupport");
        CHECK(issues[0].sinceVersion == "2.38.0");
        CHECK(issues[0].version == "2.36.1");
    }

    TEST_CASE("EngineVersionFeatures - Default profile is the latest release and reports nothing")
    {
        const auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(
            R"({"engineProperties": {"foreachSupport": true, "memberInitMode": 1}})");
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed->profile.declared);
        CHECK(parsed->profile.version == angel_lsp::config::LATEST_SUPPORTED_ENGINE_VERSION);
        CHECK(angel_lsp::config::CheckEngineProfile(*parsed).empty());
    }

    TEST_CASE("EngineVersionFeatures - A custom engine keeps engineProperties as the source of truth")
    {
        const auto parsed = angel_lsp::config::ParseEngineRuleConfigJson(
            R"({"engine": {"version": "2.20.0", "source": "custom"}, "engineProperties": {"foreachSupport": true}})");
        REQUIRE(parsed.has_value());
        CHECK(parsed->profile.source == "custom");
        CHECK(angel_lsp::config::CheckEngineProfile(*parsed).empty());
    }

    TEST_CASE("EngineVersionFeatures - Invalid version and source fall back to the defaults")
    {
        const auto parsed =
            angel_lsp::config::ParseEngineRuleConfigJson(R"({"engine": {"version": "latest", "source": "weird"}})");
        REQUIRE(parsed.has_value());
        CHECK(parsed->profile.version == angel_lsp::config::LATEST_SUPPORTED_ENGINE_VERSION);
        CHECK(parsed->profile.source == "official");
    }
}
