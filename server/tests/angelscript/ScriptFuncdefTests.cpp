#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"

TEST_SUITE("Script - Funcdefs")
{
    TEST_CASE("Funcdef declaration and delegate assignment work")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            funcdef void Callback(int);

            void MyCallback(int value) {}

            void Main()
            {
                Callback@ cb = MyCallback;
                if (cb !is null)
                {
                    cb(42);
                }
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }

    TEST_CASE("Calling delegate with incorrect signature produces error")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = R"(
            funcdef void Callback(int);

            void MyCallback(int value) {}

            void Main()
            {
                Callback@ cb = MyCallback;
                if (cb !is null)
                {
                    cb("hello"); // Error: string passed instead of int
                }
            }
        )";

        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
    }
}
