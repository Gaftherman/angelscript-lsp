#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"

TEST_SUITE("Script - Classes")
{
    TEST_CASE("Class declaration and constructor work")
    {
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

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Instance method call works")
    {
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

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Basic inheritance works")
    {
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

        auto result = fixtures::Validate(code);
        CHECK(result.IsClean());
    }
}
