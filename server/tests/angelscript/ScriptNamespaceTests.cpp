#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include "analysis/SymbolTable.h"
#include "analysis/PredefinedLoader.h"

TEST_SUITE("Script - Namespaces")
{
    TEST_CASE("Function inside namespace is called correctly")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
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

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Nested namespace function call")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
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

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Call to namespace function with incorrect arguments produces error")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            namespace Combat
            {
                void DealDamage(int amount) {}
            }
            void Main()
            {
                Combat::DealDamage("50"); // Error: string instead of int
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
    }

    TEST_CASE("Class member variable with scoped namespace type parses and resolves")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
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

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("For loop with local variable in namespace works")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
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

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Re-declaring class or enum present in as.predefined produces error diagnostic")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
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

        analysis::PredefinedLoader::LoadFromSource(predefined, engine, table, "string", "array");

        analysis::ValidationOracle oracle(engine, i18n::Locale::ES);
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

        auto diags = oracle.ValidateSync(code, "file:///main.as", nullptr);
        CHECK(!diags.empty());
    }

    TEST_CASE("Re-declaring global function present in as.predefined produces error diagnostic")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::SymbolTable table;
        std::string predefined = R"(
            namespace Combat
            {
                void DealDamage(int amount);
            }
        )";

        analysis::PredefinedLoader::LoadFromSource(predefined, engine, table, "string", "array");

        analysis::ValidationOracle oracle(engine, i18n::Locale::ES);
        std::string code = R"(
            namespace Combat
            {
                void DealDamage(int amount) {}
            }
        )";

        auto diags = oracle.ValidateSync(code, "file:///main.as", nullptr);
        CHECK(!diags.empty());
    }
}
