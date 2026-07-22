#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include "analysis/SymbolTable.h"
#include "analysis/PredefinedLoader.h"

TEST_SUITE("Script - Namespaces")
{
    TEST_CASE("Function inside namespace is called correctly")
    {
        std::string code = R"(
            namespace Combat
            {
                void DealDamage(int amount) {}
            }
            void Main()
            {
                Combat::DealDamage(50);
            }
        )";

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Nested namespace function call")
    {
        std::string code = R"(
            namespace Engine
            {
                namespace Math
                {
                    float Lerp(float a, float b, float t) { return a + (b - a) * t; }
                }
            }
            void Main()
            {
                float val = Engine::Math::Lerp(0.0f, 10.0f, 0.5f);
            }
        )";

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Class member variable with scoped namespace type parses and resolves")
    {
        std::string code = R"(
            namespace Engine
            {
                namespace Math
                {
                    class Vector3
                    {
                        float x;
                        float y;
                        float z;
                    }
                }
            }

            class Player
            {
                private Engine::Math::Vector3 position;
                private Engine::Math::Vector3 velocity;
            }

            void Main()
            {
                Player p;
            }
        )";

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("For loop with local variable in namespace works")
    {
        std::string code = R"(
            void Main()
            {
                int total = 0;
                for (int i = 0; i < 10; ++i)
                {
                    total += i;
                }
            }
        )";

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Class or enum present in as.predefined merges cleanly")
    {
        analysis::SymbolTable table;
        std::string predefined = R"(
            namespace Engine
            {
                class Vector3
                {
                    float x;
                    float y;
                    float z;
                }
                enum State
                {
                    STATE_IDLE,
                    STATE_RUNNING
                }
            }
        )";

        analysis::PredefinedLoader::LoadFromSource(predefined, table, "string", "array");

        analysis::ValidationOracle oracle(i18n::Locale::ES);
        std::string code = R"(
            namespace Engine
            {
                class Vector3
                {
                    float x;
                    float y;
                    float z;
                }
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///main.as", nullptr, &table);
        CHECK(diags.empty());
    }

    TEST_CASE("Global function present in as.predefined merges cleanly")
    {
        analysis::SymbolTable table;
        std::string predefined = R"(
            namespace Combat
            {
                void DealDamage(int amount);
            }
        )";

        analysis::PredefinedLoader::LoadFromSource(predefined, table, "string", "array");

        analysis::ValidationOracle oracle(i18n::Locale::ES);
        std::string code = R"(
            namespace Combat
            {
                void DealDamage(int amount) {}
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///main.as", nullptr, &table);
        CHECK(diags.empty());
    }
}
