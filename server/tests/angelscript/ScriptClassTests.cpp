#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"

TEST_SUITE("Script - Classes")
{
    TEST_CASE("Class declaration and constructor work")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            class Player
            {
                int hp;
                Player()
                {
                    hp = 100;
                }
            }
            void Main()
            {
                Player p;
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Instance method call works")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            class Player
            {
                int hp;
                void TakeDamage(int dmg)
                {
                    hp -= dmg;
                }
            }
            void Main()
            {
                Player p;
                p.TakeDamage(10);
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Accessing undeclared property produces error")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            class Player
            {
                int hp;
            }
            void Main()
            {
                Player p;
                p.mp = 50; // Error: mp does not exist
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
    }

    TEST_CASE("Basic inheritance works")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            class Entity
            {
                void Update() {}
            }
            
            class Player : Entity
            {
                void Move() {}
            }
            
            void Main()
            {
                Player p;
                p.Update();
                p.Move();
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }
}
