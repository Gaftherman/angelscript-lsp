#include "analysis/checkers/BinaryOperatorChecker.h"

#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SymbolCollector.h"
#include "analysis/index/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

namespace angel_lsp::test
{

TEST_SUITE("analysis/checkers/BinaryOperatorChecker")
{
    TEST_CASE("BinaryOperatorChecker - Bitwise OR between integer and string triggers as-err-no-matching-operator")
    {
        const std::string varName = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    int " +
                                 varName +
                                 " = 5 | \"hello\";\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE(
        "BinaryOperatorChecker - Bitwise OR with const int and string literal in function argument triggers error")
    {
        const std::string constName = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string callerName = GenerateRandomSymbolName();
        const std::string code = "const int " + constName +
                                 " = 1024;\n"
                                 "void " +
                                 fnName +
                                 "(int a, int b) {}\n"
                                 "void " +
                                 callerName +
                                 "() {\n"
                                 "    " +
                                 fnName + "(1, (" + constName +
                                 " | \"true\"));\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - Valid bitwise operations between integers produce no diagnostic")
    {
        const std::string v1 = GenerateRandomSymbolName();
        const std::string v2 = GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    int " +
                                 v1 +
                                 " = 5 | 3;\n"
                                 "    uint " +
                                 v2 +
                                 " = (10 & 2) ^ (1 << 4);\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - Bitwise operations with enums produce no diagnostic")
    {
        const std::string enumName = "E" + GenerateRandomSymbolName();
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "enum " + enumName +
                                 " { A = 1, B = 2 }\n"
                                 "void " +
                                 fnName +
                                 "() {\n"
                                 "    int flags = " +
                                 enumName + "::A | " + enumName +
                                 "::B;\n"
                                 "    int combined = " +
                                 enumName +
                                 "::A | 4;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - Bitwise OR on bool triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    bool b = true | false;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - Modulo with float triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    float f = 5.0f % 2.0f;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - String subtraction triggers as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    string s = \"hello\" - 5;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperatorChecker - String concatenation with int is valid and triggers no operator error")
    {
        const std::string fnName = GenerateRandomSymbolName();
        const std::string code = "void " + fnName +
                                 "() {\n"
                                 "    string s = \"hello\" + 5;\n"
                                 "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE(
        "BinaryOperator - Reverse operator overload exact match takes precedence over forward conversion (Probe A)")
    {
        const std::string leftClass = GenerateRandomSymbolName("LeftClass");
        const std::string rightClass = GenerateRandomSymbolName("RightClass");
        const std::string fnName = GenerateRandomSymbolName("TestProbeA1");
        const std::string code = "class " + rightClass + " {}\n" + "class " + leftClass + " {\n" +
                                 "    int opAdd(double d) const { return 1; }\n" + "}\n" + "class " + rightClass +
                                 "_WithRev {\n" + "    int opAdd_r(const " + leftClass +
                                 " &in l) const { return 2; }\n" + "}\n" + "void " + fnName + "() {\n" + "    " +
                                 leftClass + " l;\n" + "    " + rightClass + "_WithRev r;\n" +
                                 "    int res = l + r;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE(
        "BinaryOperator - Left operator takes precedence when both forward and reverse are exact matches (Probe A)")
    {
        const std::string leftClass = GenerateRandomSymbolName("LeftExact");
        const std::string rightClass = GenerateRandomSymbolName("RightExact");
        const std::string fnName = GenerateRandomSymbolName("TestProbeA2");
        const std::string code = "class " + rightClass + ";\n" + "class " + leftClass + " {\n" +
                                 "    int opAdd(const " + rightClass + " &in r) const { return 1; }\n" + "}\n" +
                                 "class " + rightClass + " {\n" + "    int opAdd_r(const " + leftClass +
                                 " &in l) const { return 2; }\n" + "}\n" + "void " + fnName + "() {\n" + "    " +
                                 leftClass + " l;\n" + "    " + rightClass + " r;\n" + "    int res = l + r;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Single-side opImplConv with primitive operand succeeds (Probe B)")
    {
        const std::string clsName = GenerateRandomSymbolName("ConvClass");
        const std::string fnName = GenerateRandomSymbolName("TestProbeB1");
        const std::string code = "class " + clsName + " {\n" + "    int opImplConv() const { return 42; }\n" + "}\n" +
                                 "void " + fnName + "() {\n" + "    " + clsName + " a;\n" + "    int x = a + 10;\n" +
                                 "    int y = 10 + a;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Dual-class opImplConv without primitive operand is rejected (Probe B)")
    {
        const std::string clsA = GenerateRandomSymbolName("ClassA");
        const std::string clsB = GenerateRandomSymbolName("ClassB");
        const std::string fnName = GenerateRandomSymbolName("TestProbeB2");
        const std::string code = "class " + clsA + " {\n" + "    int opImplConv() const { return 1; }\n" + "}\n" +
                                 "class " + clsB + " {\n" + "    int opImplConv() const { return 2; }\n" + "}\n" +
                                 "void " + fnName + "() {\n" + "    " + clsA + " a;\n" + "    " + clsB + " b;\n" +
                                 "    int x = a + b;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Enum bitwise operations evaluate to int")
    {
        const std::string enumName = GenerateRandomSymbolName("TestFlags");
        const std::string fnName = GenerateRandomSymbolName("TestEnumBitwise");
        const std::string code = "enum " + enumName + " { A = 1, B = 2 }\n" + "void " + fnName + "() {\n" +
                                 "    int x = " + enumName + "::A | " + enumName + "::B;\n" +
                                 "    int y = " + enumName + "::A & " + enumName + "::B;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
        CHECK_FALSE(HasDiagnostic(diags, diagnostics::codes::NoImplicitConversion));
    }

    TEST_CASE("BinaryOperator - Bitwise OR with null literal emits as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName("TestBitwiseNull");
        const std::string code = "void " + fnName + "() {\n" + "    int x = 1 | null;\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("BinaryOperator - Bitwise OR with array type emits as-err-no-matching-operator")
    {
        const std::string fnName = GenerateRandomSymbolName("TestBitwiseArray");
        const std::string code = "void " + fnName + "() {\n" + "    int x = 1 | array<int>();\n" + "}\n";
        auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasDiagnostic(diags, diagnostics::codes::NoMatchingOperator));
    }

    TEST_CASE("Indexing - Rejects non-indexable primitive and mismatched opIndex key, accepts valid containers")
    {
        const std::string fnName = GenerateRandomSymbolName("TestIndex");
        const std::string badIntCode = "void " + fnName + "() {\n    int x = 5;\n    int y = x[0];\n}\n";
        auto badIntDiags = AnalyzeScriptWithStandardProfile(badIntCode);
        CHECK(HasDiagnostic(badIntDiags, diagnostics::codes::TypeNotIndexable));

        const std::string badKeyCode =
            "void " + fnName + "() {\n    array<int> a = {1, 2, 3};\n    int x = a[\"0\"];\n}\n";
        auto badKeyDiags = AnalyzeScriptWithStandardProfile(badKeyCode);
        CHECK(HasDiagnostic(badKeyDiags, diagnostics::codes::CallNoMatchingSignature));

        const std::string clsName = GenerateRandomSymbolName("NoIdx");
        const std::string noOpClassCode =
            "class " + clsName + " {}\nvoid " + fnName + "() {\n    " + clsName + " c;\n    int z = c[0];\n}\n";
        CHECK(HasDiagnostic(AnalyzeScriptWithStandardProfile(noOpClassCode), diagnostics::codes::TypeNotIndexable));

        const std::string okCode = "void " + fnName +
                                   "() {\n"
                                   "    array<int> a = {1, 2, 3};\n"
                                   "    int x = a[0];\n"
                                   "    string s = \"abc\";\n"
                                   "    uint8 ch = s[1];\n"
                                   "    dictionary d;\n"
                                   "    d[\"k\"] = 10;\n"
                                   "}\n";
        auto okDiags = AnalyzeScriptWithStandardProfile(okCode);
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::TypeNotIndexable));
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::CallNoMatchingSignature));
    }

    TEST_CASE("Indexing - A type indexed through get_opIndex and set_opIndex accessors is indexable")
    {
        const std::string fnName = GenerateRandomSymbolName("TestAccessorIndex");
        const std::string clsName = GenerateRandomSymbolName("Row");
        const std::string cls = "class " + clsName +
                                " {\n    array<int> cells;\n"
                                "    int get_opIndex(int col) const property { return cells[col]; }\n"
                                "    void set_opIndex(int col, int val) property { cells[col] = val; }\n}\n";

        // Good: read and write through the accessor pair.
        const auto okDiags = AnalyzeScriptWithStandardProfile(cls + "void " + fnName + "() {\n    " + clsName +
                                                              " r;\n    r[3] = 42;\n    int v = r[3];\n}\n");
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::TypeNotIndexable));
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::CallNoMatchingSignature));

        // Normal: only a setter still indexes for assignment.
        const std::string setOnly =
            "class " + clsName + "Set {\n    void set_opIndex(int col, int val) property {}\n}\n";
        const auto setDiags = AnalyzeScriptWithStandardProfile(setOnly + "void " + fnName + "() {\n    " + clsName +
                                                               "Set r;\n    r[1] = 2;\n}\n");
        CHECK_FALSE(HasDiagnostic(setDiags, diagnostics::codes::TypeNotIndexable));

        // Bad: the setter's value parameter is not an index argument, so two indices do not match.
        const auto badDiags = AnalyzeScriptWithStandardProfile(cls + "void " + fnName + "() {\n    " + clsName +
                                                               " r;\n    int v = r[1, 2];\n}\n");
        CHECK((HasDiagnostic(badDiags, diagnostics::codes::CallNoMatchingSignature) ||
               HasDiagnostic(badDiags, diagnostics::codes::TypeNotIndexable)));
    }

    TEST_CASE("BinaryOperator - Compound assignment on bool is rejected while numeric and string are accepted")
    {
        const std::string fnName = GenerateRandomSymbolName("TestCompoundBool");
        const std::string badCode = "void " + fnName + "() {\n    bool b = true;\n    b += false;\n}\n";
        auto badDiags = AnalyzeScriptWithStandardProfile(badCode);
        CHECK(HasDiagnostic(badDiags, diagnostics::codes::NoMathConversion));

        const std::string okCode =
            "void " + fnName + "() {\n    int n = 1;\n    n += 2;\n    string s = \"a\";\n    s += \"b\";\n}\n";
        auto okDiags = AnalyzeScriptWithStandardProfile(okCode);
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::NoMathConversion));
    }

    TEST_CASE("BinaryOperator - Logical operators reject non-bool operands and accept bool and opImplConv")
    {
        const std::string fnName = GenerateRandomSymbolName("TestLogical");
        const std::string clsName = GenerateRandomSymbolName("BoolConv");

        const std::string badCode =
            "void " + fnName + "() {\n    int a = 1;\n    int b = 2;\n    bool c = a && b;\n}\n";
        auto badDiags = AnalyzeScriptWithStandardProfile(badCode);
        CHECK(HasDiagnostic(badDiags, diagnostics::codes::NoImplicitConversion));

        const std::string okCode = "class " + clsName + " { bool opImplConv() const { return true; } }\n" + "void " +
                                   fnName +
                                   "() {\n"
                                   "    bool x = true;\n    bool y = false;\n"
                                   "    bool r1 = x && y;\n    bool r2 = x || !y;\n    bool r3 = x ^^ y;\n"
                                   "    " +
                                   clsName + " obj;\n    bool r4 = obj && x;\n}\n";
        auto okDiags = AnalyzeScriptWithStandardProfile(okCode);
        CHECK_FALSE(HasDiagnostic(okDiags, diagnostics::codes::NoImplicitConversion));
    }
}
} // namespace angel_lsp::test
