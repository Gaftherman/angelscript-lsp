#include <doctest/doctest.h>

#include "helpers/RuleCorpusAudit.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolCollector.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SymbolTable.h"
#include "i18n/i18n.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

namespace
{
    std::vector<Diagnostic> AnalyzeOperatorSnippet(const std::string &code,
                                                   const std::string &fileUri = "file:///ops.as")
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;
        static angel_lsp::i18n::I18n i18n;

        collector.CollectSymbols(fileUri, code, parser, table);

        SemanticAnalysisRequest request{ table, fileUri, ".as.predefined", &i18n };
        request.scopeRoot = scopes.CollectScopes(code, parser);
        request.sourceCode = code;
        request.tree = parser.Parse(code);

        SemanticAnalyzer analyzer(nullptr);
        auto diags = analyzer.Analyze(request);
        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diags;
    }

    bool HasCode(const std::vector<Diagnostic> &diagnostics, const std::string &code)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [&code](const Diagnostic &diag) { return diag.code == code; });
    }

    /** @brief Wraps member declarations in a class, which is where an operator has to live. */
    std::string InClass(const std::string &members)
    {
        return "class Vec\n{\n" + members + "}\n";
    }
}

// =====================================================================================
// Return types
// =====================================================================================

TEST_CASE("OperatorRules - Reports opCmp returning something other than int")
{
    CHECK(HasCode(AnalyzeOperatorSnippet(InClass("    float opCmp(const Vec &in other) const { return 0; }\n")),
                  "as-err-opcmp-return-int"));
}

TEST_CASE("OperatorRules - opCmp returning int is accepted")
{
    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet(InClass("    int opCmp(const Vec &in other) const { return 0; }\n")),
                        "as-err-opcmp-return-int"));
}

TEST_CASE("OperatorRules - Reports opEquals returning something other than bool")
{
    CHECK(HasCode(AnalyzeOperatorSnippet(InClass("    int opEquals(const Vec &in other) const { return 0; }\n")),
                  "as-err-opequals-return-bool"));
}

TEST_CASE("OperatorRules - opEquals returning bool is accepted")
{
    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet(InClass("    bool opEquals(const Vec &in other) const { return true; }\n")),
                        "as-err-opequals-return-bool"));
}

// =====================================================================================
// Arity
// =====================================================================================

TEST_CASE("OperatorRules - Reports a binary operator taking the wrong number of arguments")
{
    CHECK(HasCode(AnalyzeOperatorSnippet(InClass("    Vec opAdd() const { return this; }\n")),
                  "as-err-binary-operator-arity"));
    CHECK(HasCode(AnalyzeOperatorSnippet(InClass("    Vec opMul(float a, float b) const { return this; }\n")),
                  "as-err-binary-operator-arity"));
}

TEST_CASE("OperatorRules - A binary operator taking one argument is accepted")
{
    const std::string members =
        "    Vec opAdd(const Vec &in other) const { return this; }\n"
        "    Vec opMul_r(float scale) const { return this; }\n"
        "    Vec@ opAssign(const Vec &in other) { return this; }\n";

    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet(InClass(members)), "as-err-binary-operator-arity"));
}

TEST_CASE("OperatorRules - Reports opIndex declared without an argument")
{
    CHECK(HasCode(AnalyzeOperatorSnippet(InClass("    float opIndex() { return 0; }\n")),
                  "as-err-opindex-no-params"));
}

TEST_CASE("OperatorRules - opIndex with an argument is accepted")
{
    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet(InClass("    float opIndex(uint i) { return 0; }\n")),
                        "as-err-opindex-no-params"));
}

// =====================================================================================
// Placement
// =====================================================================================

TEST_CASE("OperatorRules - Reports an operator declared outside a class")
{
    CHECK(HasCode(AnalyzeOperatorSnippet("int opCmp(int a) { return 0; }\n"),
                  "as-err-op-overload-global"));
}

TEST_CASE("OperatorRules - An operator inside an interface is accepted")
{
    const std::string code =
        "interface IComparable\n"
        "{\n"
        "    int opCmp(const IComparable &in other) const;\n"
        "}\n";

    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet(code), "as-err-op-overload-global"));
}

TEST_CASE("OperatorRules - A method that is not an operator is left alone")
{
    const std::string members =
        "    void Open() { }\n"
        "    int Compare(const Vec &in a, const Vec &in b) const { return 0; }\n";

    const auto diagnostics = AnalyzeOperatorSnippet(InClass(members));
    CHECK_FALSE(HasCode(diagnostics, "as-err-binary-operator-arity"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-op-overload-global"));
}

TEST_CASE("OperatorRules - A conversion operator's shape is not judged here")
{
    const std::string members =
        "    float opConv() const { return 0; }\n"
        "    int opImplConv() const { return 0; }\n";

    const auto diagnostics = AnalyzeOperatorSnippet(InClass(members));
    CHECK_FALSE(HasCode(diagnostics, "as-err-binary-operator-arity"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-opcmp-return-int"));
}

TEST_CASE("OperatorRules - A predefined stub is exempt")
{
    const auto diagnostics = AnalyzeOperatorSnippet("float opCmp(int a) { return 0; }\n",
                                                    "file:///engine.as.predefined");
    CHECK_FALSE(HasCode(diagnostics, "as-err-op-overload-global"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-opcmp-return-int"));
}

// =====================================================================================
// Out parameters (validated by FunctionRules, exercised here with the operator set)
// =====================================================================================

TEST_CASE("OperatorRules - Reports a const &out parameter")
{
    CHECK(HasCode(AnalyzeOperatorSnippet("void Fetch(const int &out value) { }\n"),
                  "as-err-const-out-param"));
}

TEST_CASE("OperatorRules - Reports an &out parameter given a default value")
{
    CHECK(HasCode(AnalyzeOperatorSnippet("void Fetch(int &out value = 0) { }\n"),
                  "as-err-out-param-default"));
}

TEST_CASE("OperatorRules - A plain &out parameter is accepted")
{
    const auto diagnostics = AnalyzeOperatorSnippet("void Fetch(int &out value) { }\n");
    CHECK_FALSE(HasCode(diagnostics, "as-err-const-out-param"));
    CHECK_FALSE(HasCode(diagnostics, "as-err-out-param-default"));
}

TEST_CASE("OperatorRules - An &out parameter defaulted to void is accepted")
{
    // AngelScript's spelling of "the caller may omit this argument and discard the value", used in
    // the engine's own documentation.
    CHECK_FALSE(HasCode(AnalyzeOperatorSnippet("void Fetch(int &out value = void) { value = 42; }\n"),
                        "as-err-out-param-default"));
}

// =====================================================================================
// AngelScript Operator Overloads Verification & Error Invariants
// =====================================================================================

TEST_CASE("OperatorRules - Binary Dual Dispatch: opAdd vs opAdd_r Resolution and Type Deduction")
{
    const std::string script =
        "class Vector2 {\n"
        "    float x, y;\n"
        "    Vector2 opAdd(float scalar) const { return Vector2(); }\n"
        "}\n"
        "class Multiplier {\n"
        "    Vector2 opAdd_r(const Vector2 &in vec) const { return Vector2(); }\n"
        "}\n"
        "void Main() {\n"
        "    Vector2 v;\n"
        "    Multiplier m;\n"
        "    Vector2 r1 = v + 5.0f;\n"
        "    Vector2 r2 = v + m;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("OperatorRules - Conditionals: Disallow bool opImplConv on Reference Types")
{
    const std::string script =
        "class RefHandleType {\n"
        "    bool opImplConv() const { return true; }\n"
        "}\n"
        "void Main() {\n"
        "    RefHandleType@ handle = RefHandleType();\n"
        "    if (handle) {\n"
        "    }\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-ref-type-bool-conv-disallowed"));
}

TEST_CASE("OperatorRules - Deleted opAssign: Explicit Deletion Diagnostic Verification")
{
    const std::string script =
        "class NonCopyable {\n"
        "    NonCopyable &opAssign(const NonCopyable &inout) delete;\n"
        "}\n"
        "void Main() {\n"
        "    NonCopyable a, b;\n"
        "    a = b;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-deleted-method-called"));
}

TEST_CASE("Auto - Deducing return types from binary dual-dispatch overloads")
{
    const std::string script =
        "class Matrix {\n"
        "    Matrix opMul(float scalar) const { return Matrix(); }\n"
        "}\n"
        "class Vector {\n"
        "    Vector opMul_r(const Matrix &in m) const { return Vector(); }\n"
        "}\n"
        "void Main() {\n"
        "    Matrix m;\n"
        "    Vector v;\n"
        "    auto res1 = m * 2.0f;\n"
        "    auto res2 = m * v;\n"
        "    Matrix m_res = res1;\n"
        "    Vector v_res = res2;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK_FALSE(HasCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasCode(diags, "as-err-cannot-infer-void"));
}

TEST_CASE("Auto - Diagnostic Parity: Cyclic dependency, void, and missing initializer")
{
    const std::string script =
        "void DoNothing() { }\n"
        "void Main() {\n"
        "    auto invalid1 = DoNothing();\n"
        "    auto invalid2 = invalid2 + 1;\n"
        "    auto invalid3;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-cannot-infer-void"));
    CHECK(HasCode(diags, "as-err-cyclic-auto-dependency"));
    CHECK(HasCode(diags, "as-err-auto-requires-initializer"));
}

TEST_CASE("Virtual Properties: Inline declaration, method pairs, and auto inference")
{
    const std::string script =
        "class Character {\n"
        "    private int m_hp;\n"
        "    int hp {\n"
        "        get const { return m_hp; }\n"
        "        set { m_hp = value; }\n"
        "    }\n"
        "}\n"
        "class Stats {\n"
        "    private float m_speed;\n"
        "    float get_speed() const property { return m_speed; }\n"
        "    void set_speed(float val) property { m_speed = val; }\n"
        "}\n"
        "void Main() {\n"
        "    Character hero;\n"
        "    hero.hp = 100;\n"
        "    auto currentHp = hero.hp;\n"
        "    int valHp = currentHp;\n"
        "    Stats stats;\n"
        "    stats.speed = 5.5f;\n"
        "    auto s = stats.speed;\n"
        "    float valSpeed = s;\n"
        "    valHp += 1;\n"
        "    valSpeed += 1.0f;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(diags.empty());
}

TEST_CASE("Diagnostics: Read-only and write-only property violations")
{
    const std::string script =
        "class Device {\n"
        "    int readOnlyProp { get const { return 42; } }\n"
        "    int writeOnlyProp { set { } }\n"
        "}\n"
        "void Main() {\n"
        "    Device dev;\n"
        "    dev.readOnlyProp = 10;\n"
        "    auto val = dev.writeOnlyProp;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-read-only-property"));
    CHECK(HasCode(diags, "as-err-write-only-property"));
}

TEST_CASE("Diagnostics: Property get/set type mismatch")
{
    const std::string script =
        "class BadObj {\n"
        "    int get_value() const property { return 0; }\n"
        "    void set_value(string val) property {}\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-property-type-mismatch"));
}

TEST_CASE("Lifetime Constraints: Compound assignment on Value Type vs Reference Type")
{
    const std::string script =
        "class ValueObject {\n"
        "    int score { get const { return 0; } set {} }\n"
        "}\n"
        "class RefObject {\n"
        "    int score { get const { return 0; } set {} }\n"
        "}\n"
        "void Main() {\n"
        "    ValueObject valObj;\n"
        "    valObj.score += 5;\n"
        "    RefObject@ refObj = RefObject();\n"
        "    refObj.score += 5;\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-compound-assign-on-value-prop"));
}

TEST_CASE("Restrictions: Increment/Decrement and Indexed Compound Assignment")
{
    const std::string script =
        "class Inventory {\n"
        "    int count { get const { return 0; } set {} }\n"
        "    string get_items(int idx) property { return \"\"; }\n"
        "    void set_items(int idx, const string &in val) property {}\n"
        "}\n"
        "void Main() {\n"
        "    Inventory inv;\n"
        "    inv.count++;\n"
        "    --inv.count;\n"
        "    inv.items[0] += \"ex\";\n"
        "}\n";

    auto diags = AnalyzeOperatorSnippet(script);
    CHECK(HasCode(diags, "as-err-inc-dec-on-virtual-prop"));
    CHECK(HasCode(diags, "as-err-compound-assign-on-indexed-prop"));
}

// =====================================================================================
// Corpus audit (opt-in - run via
// `angel_lsp_tests.exe --no-skip --test-case="*Operator Rules Corpus Audit*"`)
// =====================================================================================

TEST_CASE("OperatorRules - Operator Rules Corpus Audit" * doctest::skip(true))
{
    static const std::vector<std::string> k_codes = {
        "as-err-opcmp-return-int", "as-err-opequals-return-bool", "as-err-opindex-no-params",
        "as-err-binary-operator-arity", "as-err-op-overload-global", "as-err-const-out-param",
        "as-err-out-param-default"
    };

    const auto result = angel_lsp::test::RunCorpusAudit([](const std::string &code)
    {
        return std::find(k_codes.begin(), k_codes.end(), code) != k_codes.end();
    });

    MESSAGE("Operator-rule corpus audit: files=" << result.filesAnalysed
            << " totalFlagged=" << result.Total()
            << " seconds=" << result.seconds);

    for (const auto &[code, count] : result.countByCode)
    {
        MESSAGE("  " << code << ": " << count);
    }
    for (const auto &hit : result.hits)
    {
        MESSAGE("  " << hit.fileName << ":" << hit.line << " [" << hit.code << "] " << hit.message);
    }

    CHECK(result.filesAnalysed > 0);

    // Nothing survives triage. The one finding the rules produced before hardening was on
    // `void func(int &out output = void)`, taken verbatim from the engine's documentation - `= void`
    // is the one default an &out parameter is allowed.
    CHECK(result.Total() == 0);
}
