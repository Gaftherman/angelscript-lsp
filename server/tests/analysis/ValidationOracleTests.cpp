#include <doctest/doctest.h>
#include "helpers/TestFixtures.h"
#include <iostream>
#include "analysis/PredefinedLoader.h"
#include "analysis/SymbolTable.h"

TEST_SUITE("ValidationOracle")
{
    TEST_CASE("Collects syntax error diagnostics")
    {
        std::string code = "void Main() { int x = ; }";

        auto result = fixtures::Validate(code);
        CHECK(result.HasError());
        CHECK(!result.IsClean());
    }

    TEST_CASE("Collects semantic error diagnostics")
    {
        std::string code = "void Main() { UnknownFunctionCall(1, 2); }";

        auto result = fixtures::Validate(code);
        CHECK(result.HasError());

        bool foundSemanticError = false;
        for (const auto &d : result.diags)
        {
            if (d.message.find("Undeclared identifier or symbol") != std::string::npos)
            {
                foundSemanticError = true;
                CHECK(d.range.start.line == 0);
            }
        }
        CHECK(foundSemanticError);
    }

    TEST_CASE("Validates clean code without diagnostics")
    {
        analysis::SymbolTable globalTable;
        analysis::PredefinedLoader::LoadFromSource("void Print();", globalTable);

        std::string code = "void Main() { Print(); }";
        auto result = fixtures::Validate(code, &globalTable);
        CHECK(result.IsClean());
    }

    TEST_CASE("Translates diagnostics when locale is ES")
    {
        analysis::ValidationOracle oracle(i18n::Locale::ES);

        std::string code = "void Main() { int x = ; }";
        auto result = oracle.ValidateSync(code);

        CHECK(result.size() > 0);
    }

    TEST_CASE("Preprocessor #if WORD conditional compilation validation")
    {
        analysis::ValidationOracle oracle;

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
        analysis::ValidationOracle oracle;

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

        std::string codeUnterminated = R"script(
void Main()
{
    #if FEATURE_X
    int val = 5;
)script";
        CHECK_NOTHROW(oracle.ValidateSync(codeUnterminated));
    }

    TEST_CASE("Isolated file without #include reports error when accessing symbols from other files")
    {
        analysis::ValidationOracle oracle;

        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/fileA.as", "#include \"fileB.as\"\nvoid FuncA() { FuncB(); }"},
            {"file:///C/fileB.as", "#include \"fileC.as\"\nvoid FuncB() { FuncC(); }"},
            {"file:///C/fileC.as", "class Vector3 { float x; float y; float z; };\nvoid FuncC() {}"},
            {"file:///C/fileD.as", "void Main() { Vector3 pos; FuncA(); }"}
        };

        auto docResolver = [&](const std::string &uri) -> const Document *
        {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end())
            {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        auto diagsA = oracle.ValidateSync(inMemoryFiles["file:///C/fileA.as"], "file:///C/fileA.as", docResolver);
        CHECK(diagsA.empty());

        auto diagsD = oracle.ValidateSync(inMemoryFiles["file:///C/fileD.as"], "file:///C/fileD.as", docResolver);
        CHECK(!diagsD.empty());
    }

    TEST_CASE("Two independent #include trees (Tree 1: A<->B, Tree 2: C<->D) maintain isolated module scope")
    {
        analysis::ValidationOracle oracle;

        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/tree1_A.as", "#include \"tree1_B.as\"\nvoid RunTree1() { Tree1_Helper(); }"},
            {"file:///C/tree1_B.as", "void Tree1_Helper() {}"},
            {"file:///C/tree2_C.as", "#include \"tree2_D.as\"\nvoid RunTree2() { Tree2_Helper(); Tree1_Helper(); }"},
            {"file:///C/tree2_D.as", "void Tree2_Helper() {}"}
        };

        auto docResolver = [&](const std::string &uri) -> const Document *
        {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end())
            {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        auto diagsA = oracle.ValidateSync(inMemoryFiles["file:///C/tree1_A.as"], "file:///C/tree1_A.as", docResolver);
        CHECK(diagsA.empty());

        auto diagsC = oracle.ValidateSync(inMemoryFiles["file:///C/tree2_C.as"], "file:///C/tree2_C.as", docResolver);
        CHECK(!diagsC.empty());
    }

    TEST_CASE("Diagnostics from included files do not bleed into main file")
    {
        analysis::ValidationOracle oracle;

        std::unordered_map<std::string, std::string> inMemoryFiles = {
            {"file:///C/main.as", "#include \"math_utils.as\"\nvoid Main() { }"},
            {"file:///C/math_utils.as", "void Helper() { UnknownFunctionCall(); }"}
        };

        auto docResolver = [&](const std::string &uri) -> const Document *
        {
            static std::unordered_map<std::string, Document> docs;
            auto it = inMemoryFiles.find(uri);
            if (it != inMemoryFiles.end())
            {
                docs.insert_or_assign(uri, Document(uri, it->second));
                return &docs.at(uri);
            }
            return nullptr;
        };

        auto diagsMain = oracle.ValidateSync(inMemoryFiles["file:///C/main.as"], "file:///C/main.as", docResolver);
        CHECK(diagsMain.empty());

        auto diagsChild = oracle.ValidateSync(inMemoryFiles["file:///C/math_utils.as"], "file:///C/math_utils.as", docResolver);
        CHECK(!diagsChild.empty());
    }

    TEST_CASE("Preprocessor #include syntax validation: missing/unclosed quotes, trailing tokens, non-existent file")
    {
        analysis::ValidationOracle oracle;

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
        analysis::SymbolTable globalTable;
        analysis::PredefinedLoader::LoadFromSource("class string { string(); } class array<T> { array(); } float sqrt(float); float cos(float); float abs(float);", globalTable);
        analysis::ValidationOracle oracle;

        std::string code = R"script(
void Main()
{
    string s = "hello";
    array<int> arr = {1, 2, 3};
    float val = sqrt(16.0f) + cos(0.0f) + abs(-5.0f);
}
)script";

        auto diags = oracle.ValidateSync(code, "file:///C/main.as", nullptr, &globalTable);
        CHECK(diags.empty());
    }

    TEST_CASE("Translates #include preprocessor error diagnostics when locale is ES")
    {
        analysis::ValidationOracle oracle(i18n::Locale::ES);

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
