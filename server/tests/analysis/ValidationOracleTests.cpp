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
        for (const auto &d : result.diags)
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
            for (auto &diag : result.diags)
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
        for (const auto &d : result)
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
        for (const auto &d : result)
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

    TEST_CASE("Preprocessor #if WORD conditional compilation validation")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        // 1. Set defined words to {"DEBUG_MODE"}
        oracle.SetDefinedWords({"DEBUG_MODE"});

        std::string code = R"script(
void Main()
{
    int baseVar = 1;
    #if DEBUG_MODE
    int debugVar = 100;
    #endif

    #if UNKNOWN_DEF
    this is a broken syntax error line that must be ignored;
    #endif
}
)script";

        auto diags = oracle.ValidateSync(code);
        CHECK(diags.empty());

        // 2. Set defined words to empty
        oracle.SetDefinedWords({});

        std::string code2 = R"script(
void Main()
{
    #if DEBUG_MODE
    this is a broken syntax error line that must be ignored when DEBUG_MODE is undefined;
    #endif
}
)script";

        auto diags2 = oracle.ValidateSync(code2);
        CHECK(diags2.empty());
    }

    TEST_CASE("Preprocessor edge cases: negation, nesting, formatting and unterminated #if")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        // 1. Negated condition (#if !DEBUG_MODE)
        oracle.SetDefinedWords({"DEBUG_MODE"});
        std::string codeNegated = R"script(
void Main()
{
    #if !DEBUG_MODE
    invalid_code_that_should_be_skipped_when_DEBUG_MODE_is_defined;
    #endif
}
)script";
        CHECK(oracle.ValidateSync(codeNegated).empty());

        // When DEBUG_MODE is NOT defined, #if !DEBUG_MODE activates inner block
        oracle.SetDefinedWords({});
        std::string codeNegatedActive = R"script(
void Main()
{
    #if !DEBUG_MODE
    int releaseVar = 42;
    #endif
}
)script";
        CHECK(oracle.ValidateSync(codeNegatedActive).empty());

        // 2. Nested #if directives with whitespace
        oracle.SetDefinedWords({"FEATURE_A"});
        std::string codeNested = R"script(
void Main()
{
    #if FEATURE_A
    int a = 1;
    #  if   FEATURE_B  
    invalid_code_skipped_because_B_is_missing;
    #  endif
    int b = 2;
    #endif
}
)script";
        CHECK(oracle.ValidateSync(codeNested).empty());

        // 3. Unterminated #if at EOF (safety check)
        std::string codeUnterminated = R"script(
void Main()
{
    #if FEATURE_X
    int val = 5;
)script";
        CHECK_NOTHROW(oracle.ValidateSync(codeUnterminated));
    }

    TEST_CASE("Preprocessor conditional compilation with duplicate class declarations across #if blocks")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        std::string code = R"script(
#if USE_DIRECTX
class Renderer
{
    void DrawDirectX() {}
};
#endif

#if USE_VULKAN
class Renderer
{
    void DrawVulkan() {}
};
#endif

void DummyGlobal() {}
)script";

        // Case A: Only USE_DIRECTX defined -> clean build
        oracle.SetDefinedWords({"USE_DIRECTX"});
        CHECK(oracle.ValidateSync(code).empty());

        // Case B: Only USE_VULKAN defined -> clean build
        oracle.SetDefinedWords({"USE_VULKAN"});
        CHECK(oracle.ValidateSync(code).empty());

        // Case C: Both defined -> Duplicate class diagnostic error, no crash!
        oracle.SetDefinedWords({"USE_DIRECTX", "USE_VULKAN"});
        auto diagsBoth = oracle.ValidateSync(code);
        CHECK(!diagsBoth.empty());

        // Case D: Neither defined -> clean build (omitted both)
        oracle.SetDefinedWords({});
        CHECK(oracle.ValidateSync(code).empty());
    }

    TEST_CASE("Isolated file without #include reports error when accessing symbols from other files")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        // Map mock in-memory documents: File A, B, C, D
        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/fileA.as", "#include \"fileB.as\"\nvoid FuncA() { FuncB(); }"},
            {"file:///C/fileB.as", "#include \"fileC.as\"\nvoid FuncB() { FuncC(); }"},
            {"file:///C/fileC.as", "class Vector3 { float x; float y; float z; };\nvoid FuncC() {}"},
            {"file:///C/fileD.as", "void Main() { Vector3 pos; FuncA(); }"} // File D has NO #include!
        };

        auto docResolver = [&](const std::string &uri) -> const Document * {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end()) {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        // File A (includes B -> includes C) should validate cleanly!
        auto diagsA = oracle.ValidateSync(inMemoryFiles["file:///C/fileA.as"], "file:///C/fileA.as", docResolver);
        CHECK(diagsA.empty());

        // File D (isolated, no #include) MUST report diagnostic errors for Vector3 and FuncA!
        auto diagsD = oracle.ValidateSync(inMemoryFiles["file:///C/fileD.as"], "file:///C/fileD.as", docResolver);
        CHECK(!diagsD.empty());
    }

    TEST_CASE("Two independent #include trees (Tree 1: A<->B, Tree 2: C<->D) maintain isolated module scope")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        // Tree 1: File A includes File B
        // Tree 2: File C includes File D
        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/tree1_A.as", "#include \"tree1_B.as\"\nvoid RunTree1() { Tree1_Helper(); }"},
            {"file:///C/tree1_B.as", "void Tree1_Helper() {}"},
            {"file:///C/tree2_C.as", "#include \"tree2_D.as\"\nvoid RunTree2() { Tree2_Helper(); Tree1_Helper(); }"}, // Invalid call to Tree1_Helper
            {"file:///C/tree2_D.as", "void Tree2_Helper() {}"}
        };

        auto docResolver = [&](const std::string &uri) -> const Document * {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end()) {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        // Tree 1 (A -> B) validates cleanly!
        auto diagsA = oracle.ValidateSync(inMemoryFiles["file:///C/tree1_A.as"], "file:///C/tree1_A.as", docResolver);
        CHECK(diagsA.empty());

        // Tree 2 (C -> D) tries to call Tree1_Helper from Tree 1 -> MUST report diagnostic error!
        auto diagsC = oracle.ValidateSync(inMemoryFiles["file:///C/tree2_C.as"], "file:///C/tree2_C.as", docResolver);
        CHECK(!diagsC.empty());
    }

    TEST_CASE("Diagnostics from included files do not bleed into main file")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/main.as", "#include \"math_utils.as\"\nvoid Main() { }"},
            {"file:///C/math_utils.as", "void Helper() { UnknownFunctionCall(); }"}
        };

        auto docResolver = [&](const std::string &uri) -> const Document * {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end())
            {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        // Main file has no error itself -> diagnostics for main.as MUST be empty (no diagnostic bleed!)
        auto diagsMain = oracle.ValidateSync(inMemoryFiles["file:///C/main.as"], "file:///C/main.as", docResolver);
        CHECK(diagsMain.empty());

        // Secondary file has error -> validating math_utils.as returns error diagnostic
        auto diagsChild = oracle.ValidateSync(inMemoryFiles["file:///C/math_utils.as"], "file:///C/math_utils.as", docResolver);
        CHECK(!diagsChild.empty());
    }

    TEST_CASE("Preprocessor #include syntax validation: missing/unclosed quotes, trailing tokens, non-existent file")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine);

        // 1. Trailing garbage tokens
        std::string codeTrailing = "#include \"math.as\" extra_garbage_tokens\nvoid Main() {}";
        auto diagsTrailing = oracle.ValidateSync(codeTrailing, "file:///C/main1.as");
        CHECK(!diagsTrailing.empty());
        bool foundTrailingErr = false;
        for (const auto &d : diagsTrailing)
        {
            if (d.message.find("unexpected characters") != std::string::npos)
            {
                foundTrailingErr = true;
                break;
            }
        }
        CHECK(foundTrailingErr);

        // 2. Unclosed quote
        std::string codeUnclosed = "#include \"math.as\nvoid Main() {}";
        auto diagsUnclosed = oracle.ValidateSync(codeUnclosed, "file:///C/main2.as");
        CHECK(!diagsUnclosed.empty());
        bool foundUnclosedErr = false;
        for (const auto &d : diagsUnclosed)
        {
            if (d.message.find("unclosed path delimiter") != std::string::npos)
            {
                foundUnclosedErr = true;
                break;
            }
        }
        CHECK(foundUnclosedErr);

        // 3. Non-existent file
        std::string codeMissing = "#include \"non_existent_file_path.as\"\nvoid Main() {}";
        auto diagsMissing = oracle.ValidateSync(codeMissing, "file:///C/main3.as");
        CHECK(!diagsMissing.empty());
        bool foundMissingErr = false;
        for (const auto &d : diagsMissing)
        {
            if (d.message.find("Included file not found") != std::string::npos)
            {
                foundMissingErr = true;
                break;
            }
        }
        CHECK(foundMissingErr);
    }

    TEST_CASE("Predefined symbols string, array, and math functions are consistent across all documents")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::SymbolTable globalTable;
        analysis::PredefinedLoader::LoadFromSource("class string { string(); } class array<T> { array(); } float sqrt(float); float cos(float); float abs(float);", engine, globalTable);
        analysis::ValidationOracle oracle(engine);

        std::string code = R"script(
void Main()
{
    string s = "hello";
    array<int> arr = {1, 2, 3};
    float val = sqrt(16.0f) + cos(0.0f) + abs(-5.0f);
}
)script";

        auto diags = oracle.ValidateSync(code, "file:///C/main.as");
        CHECK(diags.empty());
    }

    TEST_CASE("Translates #include preprocessor error diagnostics when locale is ES")
    {
        fixtures::EngineGuard engine(fixtures::CreateBaseEngine());
        analysis::ValidationOracle oracle(engine, i18n::Locale::ES);

        std::string codeMissing = "#include \"missing_file.as\"\nvoid Main() {}";
        auto diagsMissing = oracle.ValidateSync(codeMissing, "file:///C/main_es.as");
        CHECK(!diagsMissing.empty());

        bool foundSpanishMsg = false;
        for (const auto &d : diagsMissing)
        {
            if (d.message.find("Archivo incluido no encontrado") != std::string::npos)
            {
                foundSpanishMsg = true;
                break;
            }
        }
        CHECK(foundSpanishMsg);
    }
}
