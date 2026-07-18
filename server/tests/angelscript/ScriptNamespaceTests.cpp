#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"

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
}
