#include <doctest/doctest.h>

#include "analysis/CallChecker.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/LocalScopeCollector.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticAnalyzer.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"
#include "parser/Primitives.h"
#include "utils/IncludeResolver.h"
#include "utils/WorkspaceIncludeGraph.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace angel_lsp::test
{
namespace
{
/**
 * @brief Analyzes a script snippet and returns collected diagnostics.
 * @param[in] code AngelScript source text.
 * @return Vector of emitted diagnostics.
 */
std::vector<analysis::Diagnostic> AnalyzeSnippet(const std::string& code)
{
    parser::AngelScriptParser parser;
    analysis::SymbolTable table;
    analysis::SymbolCollector collector(nullptr);
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
 * @brief Checks if any diagnostic matches the given code.
 * @param[in] diags List of diagnostics.
 * @param[in] code Diagnostic code string view.
 * @return True if matched.
 */
bool HasDiagCode(const std::vector<analysis::Diagnostic>& diags, std::string_view code)
{
    return std::any_of(diags.begin(), diags.end(), [code](const analysis::Diagnostic& d) { return d.code == code; });
}
} // namespace

TEST_CASE("SvenCoopRegression - Rvalue rejected for &out and &inout overloads")
{
    const std::string funcName = GenerateRandomSymbolName("FuncOut");
    const std::string typeName = GenerateRandomSymbolName("Type");

    analysis::SymbolTable table;

    // Overload 1: void Func(int &out)
    analysis::FunctionSignature sigOut;
    sigOut.returnType = "void";
    analysis::ParameterInformation pOut;
    pOut.name = "val";
    pOut.typeName = "int &out";
    pOut.baseTypeName = "int";
    pOut.isReference = true;
    pOut.modifier = analysis::ParameterModifier::Out;
    sigOut.parameters.push_back(pOut);

    analysis::Symbol symOut;
    symOut.name = funcName;
    symOut.type = analysis::SymbolType::Function;
    symOut.signature = sigOut;

    // Overload 2: void Func(int)
    analysis::FunctionSignature sigVal;
    sigVal.returnType = "void";
    analysis::ParameterInformation pVal;
    pVal.name = "val";
    pVal.typeName = "int";
    pVal.baseTypeName = "int";
    sigVal.parameters.push_back(pVal);

    analysis::Symbol symVal;
    symVal.name = funcName;
    symVal.type = analysis::SymbolType::Function;
    symVal.signature = sigVal;

    std::vector<analysis::Symbol> candidates = {symOut, symVal};

    // When argument is an L-value, both are viable, but &out is viable
    const std::vector<bool> lvalTrue = {true};
    auto resLVal = analysis::ResolveBestOverload(candidates, {"int"}, table, lvalTrue);
    CHECK(resLVal.viableCandidates.size() == 2);

    // When argument is an R-value, &out is heavily penalized so by-value overload wins
    const std::vector<bool> lvalFalse = {false};
    auto resRVal = analysis::ResolveBestOverload(candidates, {"int"}, table, lvalFalse);
    REQUIRE(resRVal.bestCandidate != nullptr);
    CHECK(resRVal.bestCandidate->GetFunction().parameters[0].modifier == analysis::ParameterModifier::None);
    CHECK(resRVal.bestScore == 0);
}

TEST_CASE("SvenCoopRegression - Template type divergence in overload resolution")
{
    const std::string funcName = GenerateRandomSymbolName("TakeArray");
    analysis::SymbolTable table;

    // Overload taking array<string>
    analysis::FunctionSignature sigStr;
    sigStr.returnType = "void";
    analysis::ParameterInformation pStr;
    pStr.name = "arr";
    pStr.typeName = "array<string>";
    pStr.baseTypeName = "array";
    pStr.templateName = "array";
    sigStr.parameters.push_back(pStr);

    analysis::Symbol symStr;
    symStr.name = funcName;
    symStr.type = analysis::SymbolType::Function;
    symStr.signature = sigStr;

    // Candidate scored against array<float>
    auto res = analysis::ResolveBestOverload({symStr}, {"array<float>"}, table);
    CHECK(res.viableCandidates.empty());
    CHECK(res.bestScore >= 999);
}

TEST_CASE("SvenCoopRegression - Const vs non-const method selection")
{
    const std::string className = GenerateRandomSymbolName("CItem");
    const std::string methodName = GenerateRandomSymbolName("GetValue");
    const std::string varName = GenerateRandomSymbolName("item");

    const std::string code = "class " + className + "\n" + "{\n" + "    int " + methodName + "() { return 1; }\n" +
                             "    int " + methodName + "() const { return 2; }\n" + "}\n" + "void TestMutable(" +
                             className + " @" + varName + ")\n" + "{\n" + "    " + varName + "." + methodName +
                             "();\n" + "}\n" + "void TestConst(const " + className + " @" + varName + ")\n" + "{\n" +
                             "    " + varName + "." + methodName + "();\n" + "}\n";

    auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::CallAmbiguous));
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::CallNoMatchingSignature));
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::ConstMethodRequired));
}

TEST_CASE("SvenCoopRegression - Scoped funcdef call return type")
{
    const std::string nsName = GenerateRandomSymbolName("CallbackNs");
    const std::string funcdefName = GenerateRandomSymbolName("OnDone");
    const std::string varName = GenerateRandomSymbolName("cb");
    const std::string resName = GenerateRandomSymbolName("val");

    const std::string code = "namespace " + nsName + "\n" + "{\n" + "    funcdef int " + funcdefName + "(int a);\n" +
                             "}\n" + "void Runner(" + nsName + "::" + funcdefName + "@ " + varName + ")\n" + "{\n" +
                             "    int " + resName + " = " + varName + "(42);\n" + "}\n";

    auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::UnknownType));
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::NoImplicitConversion));
}

TEST_CASE("SvenCoopRegression - Contextual keywords not warned as unused variables")
{
    const std::string fnName = GenerateRandomSymbolName("UseKeywords");
    const std::string code = "int " + fnName + "()\n" + "{\n" + "    int function = 1;\n" + "    int get = 2;\n" +
                             "    int set = 3;\n" + "    int shared = 4;\n" +
                             "    return function + get + set + shared;\n" + "}\n";

    auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, "as-warn-unused-variable"));
}

TEST_CASE("SvenCoopRegression - Namespaced direct constructor initialization")
{
    const std::string nsName = GenerateRandomSymbolName("ns");
    const std::string subNs = GenerateRandomSymbolName("sub");
    const std::string clsName = GenerateRandomSymbolName("Logger");
    const std::string varName = GenerateRandomSymbolName("g_logger");

    const std::string code = "namespace " + nsName + " {\n" + "namespace " + subNs + " {\n" + "    class " + clsName +
                             "\n" + "    {\n" + "        " + clsName + "(string tag) {}\n" + "    }\n" + "}\n" + "}\n" +
                             nsName + "::" + subNs + "::" + clsName + " " + varName + "(\"init\");\n";

    auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::NoMatchingConstructor));
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::CallNoMatchingSignature));
}

TEST_CASE("SvenCoopRegression - WorkspaceIncludeGraph GetForwardClosure isolated from reverse branches")
{
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / ("sven_graph_" + unique);
    std::filesystem::create_directories(tempDir);

    auto Write = [&](const std::string& name, const std::string& content)
    {
        const auto p = tempDir / name;
        std::ofstream out(p, std::ios::binary);
        out << content;
    };
    auto Path = [&](const std::string& name) { return utils::IncludeResolver::NormalizePath(tempDir / name); };

    Write("main.as", "#include \"moduleA.as\"\n#include \"moduleB.as\"\n");
    Write("moduleA.as", "#include \"subA.as\"\n");
    Write("subA.as", "// subA leaf\n");
    Write("moduleB.as", "// moduleB leaf\n");
    Write("unrelated.as", "#include \"subA.as\"\n");

    utils::WorkspaceIncludeGraph graph;
    graph.Build({utils::IncludeResolver::NormalizePath(tempDir)}, {}, ".as");

    auto fwdA = graph.GetForwardClosure(Path("moduleA.as"));
    CHECK(fwdA.size() == 2);
    CHECK(std::find(fwdA.begin(), fwdA.end(), Path("moduleA.as")) != fwdA.end());
    CHECK(std::find(fwdA.begin(), fwdA.end(), Path("subA.as")) != fwdA.end());
    CHECK(std::find(fwdA.begin(), fwdA.end(), Path("main.as")) == fwdA.end());
    CHECK(std::find(fwdA.begin(), fwdA.end(), Path("moduleB.as")) == fwdA.end());
    CHECK(std::find(fwdA.begin(), fwdA.end(), Path("unrelated.as")) == fwdA.end());

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

TEST_CASE("SvenCoopRegression - char is not a built-in primitive")
{
    CHECK_FALSE(parser::primitives::IsInteger("char"));
    CHECK_FALSE(parser::primitives::IsNumeric("char"));
    CHECK_FALSE(parser::primitives::IsPrimitive("char"));
    CHECK_FALSE(parser::primitives::IsNonNullable("char"));
}

TEST_CASE("SvenCoopRegression - Empty source code parsing returns valid tree without crashing")
{
    parser::AngelScriptParser parser;
    TSTree* tree = parser.Parse("");
    REQUIRE(tree != nullptr);
    TSNode root = ts_tree_root_node(tree);
    CHECK_FALSE(ts_node_is_null(root));
    CHECK(std::string_view(ts_node_type(root)) == "script");
    CHECK(ts_node_child_count(root) == 0);
    ts_tree_delete(tree);
}

TEST_CASE("SvenCoopRegression - Overload candidate selection with ?& in varargs")
{
    const std::string funcName = GenerateRandomSymbolName("SetTimeout");
    analysis::SymbolTable table;

    // Overload 1: void SetTimeout(const string &in, float)
    analysis::FunctionSignature sig2;
    sig2.returnType = "void";
    analysis::ParameterInformation p1;
    p1.name = "fn";
    p1.typeName = "string";
    p1.modifier = analysis::ParameterModifier::In;
    p1.isReference = true;
    sig2.parameters.push_back(p1);
    analysis::ParameterInformation p2;
    p2.name = "delay";
    p2.typeName = "float";
    sig2.parameters.push_back(p2);

    analysis::Symbol sym2;
    sym2.name = funcName;
    sym2.type = analysis::SymbolType::Function;
    sym2.signature = sig2;

    // Overload 2: void SetTimeout(const string &in, float, ?& in)
    analysis::FunctionSignature sig3 = sig2;
    analysis::ParameterInformation p3;
    p3.name = "arg1";
    p3.typeName = "?";
    p3.rawText = "?& in";
    p3.modifier = analysis::ParameterModifier::In;
    p3.isReference = true;
    sig3.parameters.push_back(p3);

    analysis::Symbol sym3;
    sym3.name = funcName;
    sym3.type = analysis::SymbolType::Function;
    sym3.signature = sig3;

    // Overload 3: void SetTimeout(const string &in, float, ?& in, ?& in)
    analysis::FunctionSignature sig4 = sig3;
    analysis::ParameterInformation p4;
    p4.name = "arg2";
    p4.typeName = "?";
    p4.rawText = "?& in";
    p4.modifier = analysis::ParameterModifier::In;
    p4.isReference = true;
    sig4.parameters.push_back(p4);

    analysis::Symbol sym4;
    sym4.name = funcName;
    sym4.type = analysis::SymbolType::Function;
    sym4.signature = sig4;

    const std::vector<analysis::Symbol> candidates = {sym2, sym3, sym4};

    // Call with 3 arguments ("Reset", 0.1f, EHandle)
    const std::string customType = GenerateRandomSymbolName("EHandle");
    auto match3 = analysis::ResolveBestOverload(candidates, {"string", "float", customType}, table);
    REQUIRE(match3.bestCandidate != nullptr);
    CHECK(match3.bestCandidate->GetFunction().parameters.size() == 3);

    // Call with 4 arguments ("Reset", 0.1f, EHandle, int)
    auto match4 = analysis::ResolveBestOverload(candidates, {"string", "float", customType, "int"}, table);
    REQUIRE(match4.bestCandidate != nullptr);
    CHECK(match4.bestCandidate->GetFunction().parameters.size() == 4);
}

TEST_CASE("SvenCoopRegression - Overload resolver handles optional parameters with defaults")
{
    const std::string funcName = GenerateRandomSymbolName("SetInterval");
    analysis::SymbolTable table;

    // void SetInterval(const string &in, float, uint repeatCount = 0)
    analysis::FunctionSignature sig;
    sig.returnType = "void";
    analysis::ParameterInformation p1;
    p1.name = "fn";
    p1.typeName = "string";
    sig.parameters.push_back(p1);

    analysis::ParameterInformation p2;
    p2.name = "interval";
    p2.typeName = "float";
    sig.parameters.push_back(p2);

    analysis::ParameterInformation p3;
    p3.name = "repeatCount";
    p3.typeName = "uint";
    p3.defaultValue = "0";
    sig.parameters.push_back(p3);

    analysis::Symbol sym;
    sym.name = funcName;
    sym.type = analysis::SymbolType::Function;
    sym.signature = sig;

    const std::vector<analysis::Symbol> candidates = {sym};

    // Called with 2 arguments (omitting repeatCount)
    auto match2 = analysis::ResolveBestOverload(candidates, {"string", "float"}, table);
    REQUIRE(match2.bestCandidate != nullptr);
    CHECK(match2.bestCandidate->name == funcName);

    // Called with 3 arguments
    auto match3 = analysis::ResolveBestOverload(candidates, {"string", "float", "uint"}, table);
    REQUIRE(match3.bestCandidate != nullptr);
    CHECK(match3.bestCandidate->name == funcName);

    // OverloadResolver index lookup
    analysis::OverloadResolver resolver;
    resolver.addFunction(sym);
    CHECK(resolver.findCandidates(funcName, 2).size() == 1);
    CHECK(resolver.findCandidates(funcName, 3).size() == 1);
    CHECK(resolver.findCandidates(funcName, 1).empty());
}

TEST_CASE("SvenCoopRegression - Named arguments in function call")
{
    const std::string fnName = GenerateRandomSymbolName("TestNamed");
    const std::string code = "void " + fnName + "(int a, int b) {}\n" + "void Main()\n" + "{\n" + "    " + fnName +
                             "(b: 2, a: 1);\n" + "}\n";

    auto diags = AnalyzeSnippet(code);
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::CallArgumentCount));
    CHECK_FALSE(HasDiagCode(diags, diagnostics::codes::CallNoMatchingSignature));
}
} // namespace angel_lsp::test
