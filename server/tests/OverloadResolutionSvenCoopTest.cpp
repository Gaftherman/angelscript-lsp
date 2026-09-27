/**
 * @file OverloadResolutionSvenCoopTest.cpp
 * @brief Empirical test suite verifying overload resolution and semantic analysis remediations
 *        for Sven Co-op scripts and meta_api::json.
 */

#include <doctest/doctest.h>

#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::test
{
namespace
{
/**
 * @brief Analyzes a script snippet with optional predefined stubs and returns all diagnostics.
 * @param[in] code AngelScript source text.
 * @param[in] predefinedCode Optional predefined stub source text.
 * @return Vector of emitted diagnostics.
 */
std::vector<analysis::Diagnostic> AnalyzeSnippetWithStubs(const std::string& code,
                                                          const std::string& predefinedCode = "")
{
    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);

    if (!predefinedCode.empty())
    {
        collector.CollectSymbols("file:///engine.as.predefined", predefinedCode, parser, table);
    }

    auto diags = collector.CollectSymbols("file:///test.as", code, parser, table);

    analysis::LocalScopeCollector scopeCollector(nullptr);
    auto scopes = scopeCollector.CollectScopes(code, parser);

    TSTree* tree = parser.Parse(code);

    analysis::SemanticAnalyzer analyzer(nullptr);
    analysis::SemanticAnalysisRequest request{table, "file:///test.as", ".as.predefined", nullptr};
    request.sourceCode = code;
    request.tree = tree;
    if (scopes)
    {
        request.mutableScopeRoot = scopes.get();
        request.scopeRoot = std::move(scopes);
    }

    auto semDiags = analyzer.Analyze(request);
    diags.insert(diags.end(), semDiags.begin(), semDiags.end());

    if (tree)
    {
        ts_tree_delete(tree);
    }

    return diags;
}

/**
 * @brief Checks if a specific diagnostic code is present in the diagnostic list.
 * @param[in] diags List of diagnostics.
 * @param[in] code Target diagnostic code.
 * @return True if code is found.
 */
bool HasDiagCode(const std::vector<analysis::Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(),
                       [code](const analysis::Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_SUITE_BEGIN("OverloadResolutionSvenCoop");

TEST_CASE("Case 1 - opAddAssign resolves unambiguously with string_t dual conversion")
{
    const std::string strTClass = GenerateRandomSymbolName("string_t");
    const std::string displayVar = GenerateRandomSymbolName("displayData");
    const std::string netnameVar = GenerateRandomSymbolName("netname");

    const std::string predefined =
        "class " + strTClass + " {\n"
        "    string opImplConv() const;\n"
        "    int opImplConv() const;\n"
        "}\n";

    const std::string script =
        "void Test(" + strTClass + " " + netnameVar + ") {\n"
        "    string " + displayVar + ";\n"
        "    " + displayVar + ".opAddAssign(" + netnameVar + ");\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 2 - Ternary expression resolves with opImplCast handle conversions")
{
    const std::string baseCls = GenerateRandomSymbolName("CBaseEntity");
    const std::string derivedCls = GenerateRandomSymbolName("CBaseMonster");
    const std::string fnRemove = GenerateRandomSymbolName("Remove");
    const std::string varChild = GenerateRandomSymbolName("child");
    const std::string varSelf = GenerateRandomSymbolName("self");

    const std::string predefined =
        "class " + baseCls + " {}\n"
        "class " + derivedCls + " {\n"
        "    " + baseCls + "@ opImplCast();\n"
        "}\n"
        "void " + fnRemove + "(" + baseCls + "@ entity);\n";

    const std::string script =
        "void Test(" + baseCls + "@ " + varChild + ", " + derivedCls + "@ " + varSelf + ") {\n"
        "    " + fnRemove + "( (" + varChild + " is null ? " + varSelf + " : " + varChild + ") );\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 3 - SetKeyvalue resolves float overload when passing float + double literal")
{
    const std::string customCls = GenerateRandomSymbolName("CBaseCustomEntity");
    const std::string vecCls = GenerateRandomSymbolName("Vector");
    const std::string varEntity = GenerateRandomSymbolName("pCustom");
    const std::string timeVar = GenerateRandomSymbolName("flTime");

    const std::string predefined =
        "class " + vecCls + " {}\n"
        "class " + customCls + " {\n"
        "    bool SetKeyvalue(const string& in key, const string& in val);\n"
        "    bool SetKeyvalue(const string& in key, const " + vecCls + "& in val);\n"
        "    bool SetKeyvalue(const string& in key, float val);\n"
        "    bool SetKeyvalue(const string& in key, int val);\n"
        "}\n";

    const std::string script =
        "void Test(" + customCls + "@ " + varEntity + ", float " + timeVar + ") {\n"
        "    " + varEntity + ".SetKeyvalue(\"kick\", " + timeVar + " + 0.1);\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Case 4 - ValueOrDefault with double literal matches float overload and assigns to float")
{
    const std::string jsonCls = GenerateRandomSymbolName("json");
    const std::string varAccuracy = GenerateRandomSymbolName("accuracy");
    const std::string arrAccuracy = GenerateRandomSymbolName("primary_accuracy");

    const std::string predefined =
        "class " + jsonCls + " {\n"
        "    " + jsonCls + "@ ValueOrDefault(const string& in k, const " + jsonCls + "@ def = null, bool r = false, bool sp = false) const;\n"
        "    bool ValueOrDefault(const string& in k, bool def, bool r = false, bool sp = false) const;\n"
        "    int ValueOrDefault(const string& in k, int def, bool r = false, bool sp = false) const;\n"
        "    float ValueOrDefault(const string& in k, float def, bool r = false, bool sp = false) const;\n"
        "    string ValueOrDefault(const string& in k, const string& in def, bool r = false, bool sp = false) const;\n"
        "}\n";

    const std::string script =
        "void Test(" + jsonCls + "@ " + varAccuracy + ") {\n"
        "    float " + arrAccuracy + ";\n"
        "    " + arrAccuracy + " = " + varAccuracy + ".ValueOrDefault(\"stand\", 0.001, false, false);\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
}

TEST_CASE("Case 5 - ValueOrDefault with bool default resolves bool and compares with == false")
{
    const std::string jsonBase = GenerateRandomSymbolName("v2_json");
    const std::string jsonDerived = GenerateRandomSymbolName("json");
    const std::string varSchema = GenerateRandomSymbolName("schema");

    const std::string predefined =
        "class " + jsonBase + " {\n"
        "    " + jsonBase + "@ ValueOrDefault(const string& in k, const " + jsonBase + "@ def = null, bool r = false) const;\n"
        "    bool ValueOrDefault(const string& in k, bool def, bool r = false) const;\n"
        "    int ValueOrDefault(const string& in k, int def, bool r = false) const;\n"
        "    string ValueOrDefault(const string& in k, const string& in def, bool r = false) const;\n"
        "}\n"
        "class " + jsonDerived + " : " + jsonBase + " {\n"
        "    " + jsonBase + "@ ValueOrDefault(const string& in k, const " + jsonBase + "@ def = null, bool r = false) const;\n"
        "    bool ValueOrDefault(const string& in k, bool def, bool r = false) const;\n"
        "    int ValueOrDefault(const string& in k, int def, bool r = false) const;\n"
        "    string ValueOrDefault(const string& in k, const string& in def, bool r = false) const;\n"
        "}\n";

    const std::string script =
        "void Test(" + jsonDerived + "@ " + varSchema + ") {\n"
        "    if (" + varSchema + ".ValueOrDefault(\"unevaluatedProperties\", true) == false) {\n"
        "    }\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-matching-operator"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 6 - Get with uint argument selects int out-param over float without ambiguity")
{
    const std::string readerCls = GenerateRandomSymbolName("ConfigReader");
    const std::string varReader = GenerateRandomSymbolName("config");
    const std::string varCap = GenerateRandomSymbolName("m_uiMaxCapacity");

    const std::string predefined =
        "class " + readerCls + " {\n"
        "    bool Get(const string& in key, int &out val) const;\n"
        "    bool Get(const string& in key, float &out val) const;\n"
        "    bool Get(const string& in key, int64 &out val) const;\n"
        "    bool Get(const string& in key, double &out val) const;\n"
        "    bool Get(const string& in key, bool &out val) const;\n"
        "    bool Get(const string& in key, string &out val) const;\n"
        "}\n";

    const std::string script =
        "void Test(" + readerCls + "@ " + varReader + ") {\n"
        "    uint " + varCap + " = 0;\n"
        "    " + varReader + ".Get(\"capacity\", " + varCap + ");\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-ambiguous"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 7 - SetPair converts float and int to const string without error")
{
    const std::string fnSetPair = GenerateRandomSymbolName("SetPair");
    const std::string varHealth = GenerateRandomSymbolName("health");
    const std::string varFlags = GenerateRandomSymbolName("spawnflags");

    const std::string predefined =
        "class string {\n"
        "    string();\n"
        "    string(const string& in szString);\n"
        "    string(double flValue);\n"
        "    string(int64 iValue);\n"
        "}\n"
        "void " + fnSetPair + "(const string& in key, const string& in val);\n";

    const std::string script =
        "void Test(float " + varHealth + ", int " + varFlags + ") {\n"
        "    " + fnSetPair + "(\"health\", " + varHealth + ");\n"
        "    " + fnSetPair + "(\"spawnflags\", " + varFlags + ");\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 8 - DispatchKeyValue with int converts to const string")
{
    const std::string edictCls = GenerateRandomSymbolName("edict_t");
    const std::string fnDispatch = GenerateRandomSymbolName("DispatchKeyValue");
    const std::string varEdict = GenerateRandomSymbolName("childEdict");
    const std::string varVal = GenerateRandomSymbolName("val");

    const std::string predefined =
        "class string {\n"
        "    string();\n"
        "    string(int64 iValue);\n"
        "}\n"
        "class " + edictCls + " {}\n"
        "bool " + fnDispatch + "(" + edictCls + "@ entity, const string& in szKeyName, const string& in szValue);\n";

    const std::string script =
        "void Test(" + edictCls + "@ " + varEdict + ", int " + varVal + ") {\n"
        "    " + fnDispatch + "(" + varEdict + ", \"$i_use_flashbang\", " + varVal + ");\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_CASE("Case 9 - Function taking const string accepts custom char class argument")
{
    const std::string fnError = GenerateRandomSymbolName("ErrorUnexpected");
    const std::string varChar = GenerateRandomSymbolName("c");

    const std::string predefined =
        "class char {\n"
        "    char();\n"
        "    char(const char& in character);\n"
        "    uint32 opImplConv() const;\n"
        "}\n"
        "class string {\n"
        "    string();\n"
        "    string(const string& in szString);\n"
        "    string(char character);\n"
        "}\n"
        "bool " + fnError + "(const string& in expected, const string& in unexpected);\n";

    const std::string script =
        "void Test(char " + varChar + ") {\n"
        "    " + fnError + "(\"token\", " + varChar + ");\n"
        "}\n";

    const auto diags = AnalyzeSnippetWithStubs(script, predefined);
    CHECK_FALSE(HasDiagCode(diags, "as-err-no-implicit-conversion"));
    CHECK_FALSE(HasDiagCode(diags, "as-err-call-no-matching-signature"));
}

TEST_SUITE_END();

} // namespace angel_lsp::test
