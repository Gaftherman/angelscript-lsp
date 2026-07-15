#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"

TEST_SUITE("ValidationOracle")
{
    TEST_CASE("Collects syntax error diagnostics")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        std::string code = "void Main() { int x = ; }"; // Syntax error
        
        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
        CHECK(!result.IsClean());
    }

    TEST_CASE("Collects semantic error diagnostics")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        engine->RegisterGlobalFunction("void Print(int)", asFUNCTION(0), asCALL_CDECL);

        std::string code = "void Main() { Print(1, 2); }"; // Semantic error
        
        auto result = fixtures::Validate(engine, code);
        CHECK(result.HasError());
        CHECK(result.HasMessage("No matching signatures"));
        
        // Check range extraction
        bool foundSemanticError = false;
        for (const auto& d : result.diags)
        {
            if (d.message.find("No matching signatures") != std::string::npos)
            {
                foundSemanticError = true;
                CHECK(d.range.start.line == 0); // "void Main() { Print(1, 2); }" is line 0
            }
        }
        CHECK(foundSemanticError);
    }

    TEST_CASE("Validates clean code without diagnostics")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        engine->RegisterGlobalFunction("void Print()", asFUNCTION(0), asCALL_CDECL);

        std::string code = "void Main() { Print(); }"; // Clean code
        auto result = fixtures::Validate(engine, code);
        CHECK(result.IsClean());
    }
}
