#include "analysis/diagnostics/Diagnostics.h"
#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <string>
#include <vector>

/**
 * Snippets in this file were run through angelscript_oracle against the real scriptarray add-on; the
 * oracle accepted every Good case and rejected every Bad one on the line the test asserts.
 */
TEST_SUITE("OracleArrayTemplateParity")
{
    /** @brief Keeps only the error diagnostics, which is the severity the oracle reports. */
    static std::vector<angel_lsp::analysis::Diagnostic> ErrorsIn(
        const std::vector<angel_lsp::analysis::Diagnostic>& all)
    {
        std::vector<angel_lsp::analysis::Diagnostic> errors;
        for (const auto& d : all)
        {
            if (d.severity == angel_lsp::analysis::DiagnosticSeverity::Error)
            {
                errors.push_back(d);
            }
        }
        return errors;
    }

    TEST_CASE("ArrayTemplate - Good - value array with initializer list, length and insertLast")
    {
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string var = angel_lsp::test::GenerateRandomSymbolName("values");
        const std::string script = "void " + fn + "() { array<int> " + var + " = {1, 2, 3}; uint n = " + var +
                                   ".length(); " + var + ".insertLast(4); int first = " + var + "[0]; }\n";
        CHECK(ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script)).empty());
    }

    TEST_CASE("ArrayTemplate - Good - array of handles and an array handle alias")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Item");
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string script =
            "class " + cls + " { int v; }\nvoid " + fn + "() { array<" + cls + "@> items; items.insertLast(" + cls +
            "()); " + cls + "@ h = items[0]; h.v = 3; array<" + cls + "@>@ alias = items; uint n = alias.length(); }\n";
        CHECK(ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script)).empty());
    }

    TEST_CASE("ArrayTemplate - Normal - array handle returned from a function and null-checked")
    {
        const std::string make = angel_lsp::test::GenerateRandomSymbolName("Make");
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string script = "array<int>@ " + make + "() { array<int> a = {1}; return a; }\nvoid " + fn +
                                   "() { array<int>@ r = " + make + "(); if (r !is null) { r.insertLast(2); } }\n";
        CHECK(ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script)).empty());
    }

    TEST_CASE("ArrayTemplate - Bad - a method the array does not have is an error on that line")
    {
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string script = "void " + fn + "() { array<int> a; a.size(); }\n";
        const auto errors = ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script));
        REQUIRE_FALSE(errors.empty());
        CHECK(errors.front().range.start.line == 0);
    }

    TEST_CASE("ArrayTemplate - Bad - inserting a string into array<int> is an error on that line")
    {
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string script = "void " + fn + "() { array<int> a; a.insertLast(\"text\"); }\n";
        const auto errors = ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script));
        REQUIRE_FALSE(errors.empty());
        CHECK(errors.front().range.start.line == 0);
    }

    TEST_CASE("ArrayTemplate - Bad - reading a handle element into int is an error on that line")
    {
        const std::string cls = angel_lsp::test::GenerateRandomSymbolName("Item");
        const std::string fn = angel_lsp::test::GenerateRandomSymbolName("Run");
        const std::string script =
            "class " + cls + " { int v; }\nvoid " + fn + "() { array<" + cls + "@> items; int x = items[0]; }\n";
        const auto errors = ErrorsIn(angel_lsp::test::AnalyzeScriptWithStandardProfile(script));
        REQUIRE_FALSE(errors.empty());
        CHECK(errors.front().range.start.line == 1);
    }
}
