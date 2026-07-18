#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include <iostream>
#include "analysis/PredefinedLoader.h"
#include "analysis/SymbolTable.h"

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

    TEST_CASE("Validates array initialization list using predefined loader list factory")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::SymbolTable table;
        // Load the dummy array type using PredefinedLoader so it registers the list factory
        bool ok = analysis::PredefinedLoader::LoadFromSource("class array<T> { uint length() const; }", engine, table, "string", "array");
        CHECK(ok);

        std::string code = "void Main() { array<int> a = {1, 2, 3}; }";
        auto result = fixtures::Validate(engine, code);
        
        if (!result.IsClean())
        {
            for (auto& diag : result.diags)
            {
                std::cout << "Diag: " << diag.message << "\n";
            }
        }
        CHECK(result.IsClean());
    }

    TEST_CASE("Translates diagnostics when locale is ES")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        
        // ValidationOracle defaults to EN, but we inject ES for this test
        analysis::ValidationOracle oracle(engine, i18n::Locale::ES);
        
        // This will trigger "Expected expression value" (syntax error)
        std::string code = "void Main() { int x = ; }"; 
        auto result = oracle.ValidateSync(code);
        
        CHECK(result.size() > 0);
        
        bool foundTranslated = false;
        for (const auto& d : result)
        {
            if (d.message == "Se esperaba un valor de expresión" || d.message.find("Se esperaba") != std::string::npos)
            {
                foundTranslated = true;
                break;
            }
        }
        CHECK(foundTranslated);
    }

    TEST_CASE("Translates regex parameterized diagnostics when locale is ES")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        engine->RegisterGlobalFunction("void Print(int)", asFUNCTION(0), asCALL_CDECL);
        
        analysis::ValidationOracle oracle(engine, i18n::Locale::ES);
        
        // This will trigger "No matching signatures to 'Print(const int)'"
        // And should be translated via regex to "No hay firmas coincidentes para la función 'Print(const int)'"
        std::string code = "void Main() { Print(1, 2); }";
        auto result = oracle.ValidateSync(code);
        
        CHECK(result.size() > 0);
        
        bool foundRegexTranslated = false;
        for (const auto& d : result)
        {
            std::cout << "DIAG: " << d.message << "\n";
            if (d.message.find("No hay firmas coincidentes para la función") != std::string::npos && 
                d.message.find("Print") != std::string::npos)
                {
                foundRegexTranslated = true;
                break;
            }
        }
        CHECK(foundRegexTranslated);
    }
}
