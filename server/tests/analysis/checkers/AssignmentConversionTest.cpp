#include "analysis/diagnostics/DiagnosticCodes.h"
#include "helpers/TestUtils.h"

#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::test
{

namespace
{

int LineOf(std::string_view source, std::string_view needle)
{
    const size_t pos = source.find(needle);
    REQUIRE(pos != std::string_view::npos);
    return static_cast<int>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(pos), '\n'));
}

bool HasCode(const std::vector<analysis::Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [&](const analysis::Diagnostic& d) { return d.code == code; });
}

bool HasCodeOnLine(const std::vector<analysis::Diagnostic>& diags, std::string_view code, int line)
{
    return std::any_of(diags.begin(), diags.end(), [&](const analysis::Diagnostic& d)
                       { return d.code == code && static_cast<int>(d.range.start.line) == line; });
}

} // namespace

TEST_SUITE("analysis/checkers/AssignmentConversion")
{
    TEST_CASE("AssignmentConversion - A wildcard out conversion supports explicit construction")
    {
        const auto valueType = GenerateRandomSymbolName("DictionaryValue");
        const auto dictionaryType = GenerateRandomSymbolName("Dictionary");
        const auto targetType = GenerateRandomSymbolName("EHandle");
        const auto test = GenerateRandomSymbolName("Test");
        const auto dictionary = GenerateRandomSymbolName("dictionary");
        const auto result = GenerateRandomSymbolName("result");
        const std::string body = "class " + dictionaryType + " { " + valueType +
                                 "& opIndex(const string &in); }\nclass " + targetType + " {}\nvoid " + test + "(" +
                                 dictionaryType + "@ " + dictionary + ") { " + targetType + " " + result + " = " +
                                 targetType + "(" + dictionary + "[\"k\"]); }";
        const std::string prefix = "class " + valueType + " { void opConv(";
        const auto accepted = AnalyzeScriptWithStandardProfile(prefix + "?&out); }\n" + body);
        CHECK_FALSE(HasCode(accepted, diagnostics::codes::NoExplicitConversion));

        const auto rejected = AnalyzeScriptWithStandardProfile(prefix + "int&out); }\n" + body);
        CHECK(HasCode(rejected, diagnostics::codes::NoExplicitConversion));
    }

    TEST_CASE("AssignmentConversion - Property setter matrix validates RHS via set_ overloads")
    {
        const std::string voidFn = GenerateRandomSymbolName();
        const std::string floatFn = GenerateRandomSymbolName();
        const std::string cbType = GenerateRandomSymbolName();
        const std::string cbFn = GenerateRandomSymbolName();
        const std::string otherCls = GenerateRandomSymbolName();
        const std::string propOwner = GenerateRandomSymbolName();
        const std::string derivedOwner = GenerateRandomSymbolName();
        const std::string propName = GenerateRandomSymbolName();
        const std::string idxProp = GenerateRandomSymbolName();

        const std::string code = "funcdef void " + cbType +
                                 "();\n"
                                 "void " +
                                 voidFn +
                                 "() {}\n"
                                 "float " +
                                 floatFn + "() { return 1.5f; }\n" + cbType + "@ " + cbFn +
                                 "() { return null; }\n"
                                 "class " +
                                 otherCls +
                                 " {}\n"
                                 "class " +
                                 propOwner +
                                 " {\n"
                                 "    private string _v;\n"
                                 "    string get_" +
                                 propName +
                                 "() const property { return _v; }\n"
                                 "    void set_" +
                                 propName +
                                 "(const string&in v) property { _v = v; }\n"
                                 "    string get_" +
                                 idxProp +
                                 "(int i) const property { return _v; }\n"
                                 "    void set_" +
                                 idxProp +
                                 "(int i, const string&in v) property { _v = v; }\n"
                                 "}\n"
                                 "class " +
                                 derivedOwner + " : " + propOwner +
                                 " {}\n"
                                 "void Test() {\n"
                                 "    " +
                                 derivedOwner + " d;\n" + "    " + otherCls + "@ o = " + otherCls +
                                 "();\n"
                                 "    d." +
                                 propName + " = " + voidFn +
                                 "(); // L_VOID\n"
                                 "    d." +
                                 propName + " = " + floatFn +
                                 "(); // L_FLOAT\n"
                                 "    d." +
                                 propName + " = " + cbFn +
                                 "(); // L_FUNC\n"
                                 "    d." +
                                 propName +
                                 " = @o; // L_OTHER\n"
                                 "    d." +
                                 idxProp + "[0] = " + floatFn +
                                 "(); // L_IDX_BAD\n"
                                 "    d." +
                                 propName +
                                 " = \"ok\"; // L_OK\n"
                                 "    d." +
                                 idxProp +
                                 "[0] = \"ok\"; // L_IDX_OK\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_VOID")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_FLOAT")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_FUNC")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_OTHER")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_IDX_BAD")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_OK")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// L_IDX_OK")));
    }

    TEST_CASE(
        "AssignmentConversion - Destinations (local, global, field, base field, array element, typedef, namespace)")
    {
        const std::string voidFn = GenerateRandomSymbolName();
        const std::string cbType = GenerateRandomSymbolName();
        const std::string cbFn = GenerateRandomSymbolName();
        const std::string baseCls = GenerateRandomSymbolName();
        const std::string subCls = GenerateRandomSymbolName();
        const std::string nsName = GenerateRandomSymbolName();
        const std::string enumName = GenerateRandomSymbolName();
        const std::string tdName = GenerateRandomSymbolName();
        const std::string gVar = GenerateRandomSymbolName();

        const std::string code = "enum " + enumName +
                                 " { Red, Green }\n"
                                 "typedef int " +
                                 tdName +
                                 ";\n"
                                 "funcdef void " +
                                 cbType +
                                 "();\n"
                                 "void " +
                                 voidFn + "() {}\n" + cbType + "@ " + cbFn +
                                 "() { return null; }\n"
                                 "int " +
                                 gVar +
                                 " = 0;\n"
                                 "namespace " +
                                 nsName +
                                 " { string nsStr; }\n"
                                 "class " +
                                 baseCls +
                                 " { string baseStr; int baseInt; }\n"
                                 "class " +
                                 subCls + " : " + baseCls +
                                 " {}\n"
                                 "void Test() {\n"
                                 "    " +
                                 subCls +
                                 " s;\n"
                                 "    string loc;\n" +
                                 "    " + tdName +
                                 " td = 0;\n"
                                 "    array<int> arr = {1, 2};\n"
                                 "    array<float> fltArr = {1.0f};\n"
                                 "    loc = " +
                                 voidFn +
                                 "(); // BAD_LOC_VOID\n"
                                 "    loc = " +
                                 cbFn + "(); // BAD_LOC_CB\n" + "    " + gVar + " = " + voidFn +
                                 "(); // BAD_GLOBAL_VOID\n"
                                 "    s.baseStr = " +
                                 voidFn +
                                 "(); // BAD_BASE_VOID\n"
                                 "    s.baseInt = \"nope\"; // BAD_BASE_STR\n"
                                 "    arr[0] = " +
                                 voidFn +
                                 "(); // BAD_ARR_VOID\n"
                                 "    arr[0] = \"nope\"; // BAD_ARR_STR\n"
                                 "    arr = fltArr; // BAD_ARR_TYPE\n"
                                 "    td = \"nope\"; // BAD_TYPEDEF_STR\n" +
                                 "    " + nsName + "::nsStr = " + cbFn +
                                 "(); // BAD_NS_CB\n"
                                 "    " +
                                 gVar + " = " + enumName +
                                 "::Red; // OK_ENUM_TO_INT\n"
                                 "    td = 42; // OK_TYPEDEF_INT\n"
                                 "    loc = 1.5f; // OK_STR_FLOAT\n"
                                 "    s.baseInt = 3; // OK_BASE_INT\n"
                                 "    arr[0] = 7; // OK_ARR_INT\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_LOC_VOID")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_LOC_CB")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_GLOBAL_VOID")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_BASE_VOID")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_BASE_STR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ARR_VOID")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ARR_STR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ARR_TYPE")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_TYPEDEF_STR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_NS_CB")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_ENUM_TO_INT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_TYPEDEF_INT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_STR_FLOAT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_BASE_INT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_ARR_INT")));
    }

    TEST_CASE("AssignmentConversion - opAssign, converting ctors, opImplConv, ternary, chained calls, template, null, "
              "funcdef")
    {
        const std::string opBox = GenerateRandomSymbolName();
        const std::string ctorBox = GenerateRandomSymbolName();
        const std::string expCtorBox = GenerateRandomSymbolName();
        const std::string convSrc = GenerateRandomSymbolName();
        const std::string innerCls = GenerateRandomSymbolName();
        const std::string outerCls = GenerateRandomSymbolName();
        const std::string tplCls = GenerateRandomSymbolName();
        const std::string fnVal = GenerateRandomSymbolName();

        const std::string code = "void " + fnVal +
                                 "() {}\n"
                                 "class " +
                                 opBox + " { " + opBox +
                                 "& opAssign(float v) { return this; } }\n"
                                 "class " +
                                 ctorBox + " { " + ctorBox + "() {} " + ctorBox +
                                 "(int v) {} }\n"
                                 "class " +
                                 expCtorBox + " { " + expCtorBox + "() {} " + expCtorBox +
                                 "(int v) explicit {} }\n"
                                 "class " +
                                 convSrc +
                                 " { int opImplConv() const { return 1; } }\n"
                                 "class " +
                                 innerCls +
                                 " { void getVoid() {} string getText() { return \"x\"; } }\n"
                                 "class " +
                                 outerCls + " { " + innerCls + " makeInner() { return " + innerCls + "(); } }\n" +
                                 outerCls + " makeOuter() { return " + outerCls +
                                 "(); }\n"
                                 "class " +
                                 tplCls +
                                 "<T> { T value; }\n"
                                 "void Test() {\n"
                                 "    " +
                                 opBox +
                                 " ob;\n"
                                 "    ob = 1.5f; // OK_OPASSIGN\n"
                                 "    ob = \"bad\"; // BAD_OPASSIGN_STR\n"
                                 "    " +
                                 ctorBox +
                                 " cb;\n"
                                 "    cb = 7; // OK_CTOR\n"
                                 "    " +
                                 expCtorBox +
                                 " eb;\n"
                                 "    eb = 7; // BAD_EXPLICIT_CTOR\n"
                                 "    int x = 0;\n" +
                                 "    " + convSrc +
                                 " cs;\n"
                                 "    x = cs; // OK_OPIMPLCONV\n"
                                 "    x = true ? 1 : \"bad\"; // BAD_TERNARY_MIXED\n"
                                 "    x = " +
                                 fnVal +
                                 "; // BAD_FUNC_VALUE\n"
                                 "    x = @" +
                                 fnVal +
                                 "; // BAD_FUNC_ADDR\n"
                                 "    x = null; // BAD_NULL_INT\n"
                                 "    string s;\n"
                                 "    s = makeOuter().makeInner().getVoid(); // BAD_CHAIN_VOID\n"
                                 "    s = makeOuter().makeInner().getText(); // OK_CHAIN_TEXT\n"
                                 "    " +
                                 tplCls +
                                 "<int> ti;\n"
                                 "    " +
                                 tplCls +
                                 "<float> tf;\n"
                                 "    ti = tf; // BAD_TEMPLATE_MISMATCH\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_OPASSIGN")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_OPASSIGN_STR")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_CTOR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_EXPLICIT_CTOR")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_OPIMPLCONV")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_TERNARY_MIXED")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_FUNC_VALUE")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_FUNC_ADDR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NullNonHandle, LineOf(code, "// BAD_NULL_INT")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_CHAIN_VOID")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_CHAIN_TEXT")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_TEMPLATE_MISMATCH")));
    }

    TEST_CASE("AssignmentConversion - Compound operators, enums, bitwise ops, and multi-line RHS anchor")
    {
        const std::string addCls = GenerateRandomSymbolName();
        const std::string plainCls = GenerateRandomSymbolName();
        const std::string enumName = GenerateRandomSymbolName();
        const std::string voidFn = GenerateRandomSymbolName();

        const std::string code = "void " + voidFn +
                                 "() {}\n"
                                 "enum " +
                                 enumName +
                                 " { One }\n"
                                 "class " +
                                 addCls + " { " + addCls +
                                 "& opAddAssign(float v) { return this; } }\n"
                                 "class " +
                                 plainCls +
                                 " {}\n"
                                 "void Test() {\n"
                                 "    " +
                                 addCls +
                                 " ac;\n"
                                 "    ac += 2.0f; // OK_ADDASSIGN\n"
                                 "    ac += \"bad\"; // BAD_ADDASSIGN_STR\n"
                                 "    " +
                                 plainCls +
                                 " p;\n"
                                 "    p += p; // BAD_NO_OPADDASSIGN\n"
                                 "    " +
                                 enumName + " e = " + enumName +
                                 "::One;\n"
                                 "    e += " +
                                 enumName +
                                 "::One; // BAD_ENUM_COMPOUND\n"
                                 "    int bits = 1;\n"
                                 "    bits &= 1.5f; // BAD_BITWISE_FLOAT\n"
                                 "    bool flag = false;\n"
                                 "    flag += 1; // BAD_BOOL_COMPOUND\n"
                                 "    float flt = 0.0f;\n"
                                 "    flt =\n\n"
                                 "        " +
                                 voidFn +
                                 "(); // BAD_MULTILINE_RHS\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_ADDASSIGN")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ADDASSIGN_STR")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_NO_OPADDASSIGN")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ENUM_COMPOUND")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_BITWISE_FLOAT")));
        CHECK(HasCode(diags, diagnostics::codes::NoMathConversion));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_MULTILINE_RHS")));
    }

    TEST_CASE("AssignmentConversion - Undeclared host types and classes with unknown base remain exempt except void")
    {
        const std::string subCls = GenerateRandomSymbolName();
        const std::string hostBase = GenerateRandomSymbolName();
        const std::string voidFn = GenerateRandomSymbolName();

        const std::string code = "void " + voidFn +
                                 "() {}\n"
                                 "class " +
                                 subCls + " : " + hostBase +
                                 " {}\n"
                                 "void Test(" +
                                 hostBase +
                                 "@ h) {\n"
                                 "    " +
                                 subCls +
                                 " s;\n"
                                 "    s = h; // OK_HOST_HIERARCHY\n"
                                 "    s = " +
                                 voidFn +
                                 "(); // BAD_HOST_VOID\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK_FALSE(
            HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_HOST_HIERARCHY")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_HOST_VOID")));
    }

    TEST_CASE("AssignmentConversion - any-like class with explicit constructor blocks implicit int conversion while "
              "allowing explicit construction")
    {
        const std::string boxCls = GenerateRandomSymbolName("AnyLike");
        const std::string takeFn = GenerateRandomSymbolName("TakeAnyLike");

        const std::string code = "class " + boxCls +
                                 " {\n"
                                 "    " +
                                 boxCls +
                                 "() {}\n"
                                 "    " +
                                 boxCls +
                                 "(int v) explicit {}\n"
                                 "    " +
                                 boxCls + "& opAssign(const " + boxCls +
                                 "&in other) { return this; }\n"
                                 "}\n"
                                 "void " +
                                 takeFn + "(" + boxCls +
                                 " v) {}\n"
                                 "void Test() {\n"
                                 "    " +
                                 boxCls +
                                 " badInit = 5; // BAD_INIT\n"
                                 "    " +
                                 boxCls +
                                 " b;\n"
                                 "    b = 5; // BAD_ASSIGN\n"
                                 "    " +
                                 takeFn +
                                 "(5); // BAD_CALL\n"
                                 "    " +
                                 boxCls +
                                 " okDirect(5); // OK_DIRECT\n"
                                 "    " +
                                 boxCls + "@ okConstruct = " + boxCls +
                                 "(5); // OK_CONSTRUCT\n"
                                 "    " +
                                 takeFn + "(" + boxCls +
                                 "(5)); // OK_CALL\n"
                                 "    b = okDirect; // OK_ASSIGN_SAME\n"
                                 "}\n";

        const auto diags = AnalyzeScriptWithStandardProfile(code);
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_INIT")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_ASSIGN")));
        CHECK(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// BAD_CALL")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_DIRECT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_CONSTRUCT")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_CALL")));
        CHECK_FALSE(HasCodeOnLine(diags, diagnostics::codes::NoImplicitConversion, LineOf(code, "// OK_ASSIGN_SAME")));
        CHECK_FALSE(HasCode(diags, "as-err-no-explicit-conversion"));
        CHECK_FALSE(HasCode(diags, "as-err-no-matching-constructor"));
    }
}

} // namespace angel_lsp::test
