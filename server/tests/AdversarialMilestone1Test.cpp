#include <doctest/doctest.h>
#include "analysis/SemanticHelpers.h"
#include "analysis/OverloadResolver.h"
#include "analysis/DefiniteAssignmentChecker.h"
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
    const Scope *FindInnermostScope(const Scope *root, uint32_t line, uint32_t character)
    {
        if (!root)
        {
            return nullptr;
        }

        const auto contains = [line, character](const Scope &scope)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        };

        if (!contains(*root))
        {
            return nullptr;
        }

        const Scope *current = root;
        for (bool descended = true; descended;)
        {
            descended = false;
            for (const auto &child : current->children)
            {
                if (child && contains(*child))
                {
                    current = child.get();
                    descended = true;
                    break;
                }
            }
        }
        return current;
    }

    void TraverseForExpressions(TSNode node, TSNode &lastExpr)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        std::string_view stmtType = ts_node_type(node);
        if ((stmtType == "expression_statement" || stmtType == "return_statement") && ts_node_named_child_count(node) > 0)
        {
            lastExpr = ts_node_named_child(node, 0);
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TraverseForExpressions(ts_node_named_child(node, i), lastExpr);
        }
    }

    std::string DeduceTypeAtLastExpression(const std::string &code)
    {
        AngelScriptParser parser;
        SymbolCollector collector(nullptr);
        LocalScopeCollector scopes(nullptr);
        SymbolTable table;

        const std::string fileUri = "file:///adv_test.as";
        collector.CollectSymbols(fileUri, code, parser, table);
        auto scopeRoot = scopes.CollectScopes(code, parser);
        TSTree *tree = parser.Parse(code);

        std::string result;
        if (tree)
        {
            TSNode root = ts_tree_root_node(tree);
            TSNode lastExpr{};
            TraverseForExpressions(root, lastExpr);

            if (!ts_node_is_null(lastExpr))
            {
                TSPoint pt = ts_node_start_point(lastExpr);
                const Scope *innerScope = FindInnermostScope(scopeRoot.get(), pt.row, pt.column);
                result = ResolveExpressionType(lastExpr, innerScope ? innerScope : scopeRoot.get(), table, code, fileUri);
            }
            ts_tree_delete(tree);
        }
        return result;
    }

    std::vector<Diagnostic> AnalyzeCodeForDiagnostics(const std::string &code, const std::string &fileUri = "file:///adv_assign.as")
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
        auto diagnostics = analyzer.Analyze(request);

        if (request.tree)
        {
            ts_tree_delete(const_cast<TSTree *>(request.tree));
        }
        return diagnostics;
    }

    bool HasUninitializedRead(const std::vector<Diagnostic> &diagnostics)
    {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                           [](const Diagnostic &d) { return d.code == "as-err-uninitialized-variable-read"; });
    }

    ParameterInformation MakeParam(std::string typeName, std::string name, bool isHandle = false, bool isConst = false, ParameterModifier mod = ParameterModifier::None)
    {
        ParameterInformation p;
        p.typeName = std::move(typeName);
        p.name = std::move(name);
        p.isHandle = isHandle;
        p.isConst = isConst;
        p.modifier = mod;
        p.rawText = p.typeName + " " + p.name;
        return p;
    }
}

TEST_SUITE("Adversarial_ExpressionTypeResolver")
{
    TEST_CASE("Null Node and Empty Source Code Safety")
    {
        SymbolTable table;
        TSNode nullNode{};
        CHECK(ResolveExpressionType(nullNode, nullptr, table, "") == "");
        CHECK(ResolveExpressionType(nullNode, nullptr, table, "int x = 5;") == "");
    }

    TEST_CASE("Malformed Incomplete Syntax Does Not Crash")
    {
        // Trailing operator
        CHECK_NOTHROW(DeduceTypeAtLastExpression("void main() { 10 + ; }"));
        // Broken member access
        CHECK_NOTHROW(DeduceTypeAtLastExpression("void main() { string s; s. ; }"));
        // Broken call
        CHECK_NOTHROW(DeduceTypeAtLastExpression("void main() { func( ; }"));
        // Broken ternary
        CHECK_NOTHROW(DeduceTypeAtLastExpression("void main() { true ? 1 : ; }"));
        // Broken index
        CHECK_NOTHROW(DeduceTypeAtLastExpression("void main() { array<int> a; a[ ; }"));
    }

    TEST_CASE("Unresolvable Types and Missing Symbols Return Gracefully")
    {
        CHECK(DeduceTypeAtLastExpression("void main() { nonExistentVar + 10; }") == "");
        CHECK(DeduceTypeAtLastExpression("void main() { string s; s.nonExistentMethod(); }") == "");
        CHECK(DeduceTypeAtLastExpression("void main() { a.b.c.d.e(); }") == "");
    }

    TEST_CASE("Template Parsing Edge Cases and Malformed Strings")
    {
        auto t1 = ParseTemplateType("");
        CHECK(t1.containerName == "");
        CHECK(t1.templateArgs.empty());

        auto t2 = ParseTemplateType("   ");
        CHECK(t2.containerName == "");
        CHECK(t2.templateArgs.empty());

        auto t3 = ParseTemplateType("int");
        CHECK(t3.containerName == "int");
        CHECK(t3.templateArgs.empty());

        auto t4 = ParseTemplateType("array<int");
        CHECK(t4.containerName == "array<int");
        CHECK(t4.templateArgs.empty());

        auto t5 = ParseTemplateType("array<int, >");
        CHECK(t5.containerName == "array");
        REQUIRE(t5.templateArgs.size() == 1);
        CHECK(t5.templateArgs[0] == "int");

        auto t6 = ParseTemplateType("const dictionary<string, array<int>>@");
        CHECK(t6.containerName == "dictionary");
        REQUIRE(t6.templateArgs.size() == 2);
        CHECK(t6.templateArgs[0] == "string");
        CHECK(t6.templateArgs[1] == "array<int>");

        auto t7 = ParseTemplateType("map<int, map<string, array<float>>>");
        CHECK(t7.containerName == "map");
        REQUIRE(t7.templateArgs.size() == 2);
        CHECK(t7.templateArgs[0] == "int");
        CHECK(t7.templateArgs[1] == "map<string, array<float>>");
    }

    TEST_CASE("Deep Cyclic and Self-Referential Method Chains")
    {
        std::string cyclicCode =
            "class Node\n"
            "{\n"
            "    Node@ next() { return null; }\n"
            "    int value;\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    Node n;\n"
            "    n.next().next().next().next().value;\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(cyclicCode) == "int");

        std::string pingPongCode =
            "class B;\n"
            "class A\n"
            "{\n"
            "    B@ toB() { return null; }\n"
            "}\n"
            "class B\n"
            "{\n"
            "    A@ toA() { return null; }\n"
            "    float weight;\n"
            "}\n"
            "void main()\n"
            "{\n"
            "    A a;\n"
            "    a.toB().toA().toB().weight;\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(pingPongCode) == "float");
    }

    TEST_CASE("Deeply Nested Generic Template Indexing")
    {
        std::string code =
            "void main()\n"
            "{\n"
            "    array<array<array<int>>> cube;\n"
            "    cube[0][1][2];\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(code) == "int");

        std::string dictMapCode =
            "void main()\n"
            "{\n"
            "    dictionary<string, array<string>> lookup;\n"
            "    lookup[\"key\"][0];\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(dictMapCode) == "string");
    }

    TEST_CASE("CleanBaseType Edge Cases")
    {
        CHECK(CleanBaseType("") == "");
        CHECK(CleanBaseType("   ") == "");
        CHECK(CleanBaseType("const int@&") == "int");
        CHECK(CleanBaseType("array<const string@>") == "string");
        CHECK(CleanBaseType("array<array<int>>") == "int");
        CHECK(CleanBaseType("int[][][]") == "int");
    }

    TEST_CASE("Lexical Shadowing In Nested Blocks")
    {
        std::string shadowCode =
            "void main()\n"
            "{\n"
            "    int x = 10;\n"
            "    {\n"
            "        float x = 2.5f;\n"
            "        x;\n"
            "    }\n"
            "}\n";
        CHECK(DeduceTypeAtLastExpression(shadowCode) == "float");
    }

    TEST_CASE("Circular Class Inheritance Does Not Hang")
    {
        SymbolTable table;
        Symbol symA;
        symA.name = "ClassA";
        symA.type = SymbolType::Class;
        ClassSignature clsA;
        clsA.bases = { "ClassB" };
        symA.signature = clsA;

        Symbol symB;
        symB.name = "ClassB";
        symB.type = SymbolType::Class;
        ClassSignature clsB;
        clsB.bases = { "ClassA" };
        symB.signature = clsB;

        table.AddSymbol(symA);
        table.AddSymbol(symB);

        auto hierarchy = GetInheritedTypeHierarchy("ClassA", table);
        CHECK(hierarchy.size() == 2);
    }
}

TEST_SUITE("Adversarial_OverloadResolver")
{
    TEST_CASE("Argument Count Exceeds Candidate Max Arity")
    {
        SymbolTable table;
        Symbol sym;
        sym.name = "Foo";
        sym.type = SymbolType::Function;
        FunctionSignature sig;
        sig.returnType = "void";
        sig.parameters = { MakeParam("int", "a") };
        sym.signature = sig;

        std::vector<Symbol> candidates = { sym };
        // 0 arguments (required 1)
        auto res0 = ResolveBestOverload(candidates, {}, table);
        CHECK(res0.bestCandidate == nullptr);
        CHECK(res0.viableCandidates.empty());

        // 2 arguments (max 1)
        auto res2 = ResolveBestOverload(candidates, {"int", "int"}, table);
        CHECK(res2.bestCandidate == nullptr);
        CHECK(res2.viableCandidates.empty());

        // 5 arguments (max 1)
        auto res5 = ResolveBestOverload(candidates, {"int", "int", "int", "int", "int"}, table);
        CHECK(res5.bestCandidate == nullptr);
        CHECK(res5.viableCandidates.empty());
    }

    TEST_CASE("Candidate With Empty or Invalid Signatures")
    {
        SymbolTable table;
        Symbol invalidSym1;
        invalidSym1.name = "Bad1";
        invalidSym1.type = SymbolType::Variable; // Not a function

        Symbol invalidSym2;
        invalidSym2.name = "Bad2";
        invalidSym2.type = SymbolType::Function;
        invalidSym2.signature = VariableSignature{}; // wrong variant

        Symbol validSym;
        validSym.name = "Good";
        validSym.type = SymbolType::Function;
        FunctionSignature sig;
        sig.returnType = "int";
        sig.parameters = { MakeParam("int", "x") };
        validSym.signature = sig;

        std::vector<Symbol> candidates = { invalidSym1, invalidSym2, validSym };
        auto res = ResolveBestOverload(candidates, {"int"}, table);
        REQUIRE(res.bestCandidate != nullptr);
        CHECK(res.bestCandidate->name == "Good");
        CHECK(res.viableCandidates.size() == 1);
    }

    TEST_CASE("Variadic Wildcard Overload Resolution")
    {
        SymbolTable table;
        Symbol symVar;
        symVar.name = "Printf";
        symVar.type = SymbolType::Function;
        FunctionSignature sigVar;
        sigVar.returnType = "void";
        auto p1 = MakeParam("string", "fmt");
        auto p2 = MakeParam("?&in", "args");
        p2.rawText = "...?&in";
        sigVar.parameters = { p1, p2 };
        symVar.signature = sigVar;

        std::vector<Symbol> candidates = { symVar };

        // 1 arg (fmt only)
        auto match1 = ResolveBestOverload(candidates, {"string"}, table);
        REQUIRE(match1.bestCandidate != nullptr);

        // 4 args (fmt + 3 extra)
        auto match4 = ResolveBestOverload(candidates, {"string", "int", "float", "bool"}, table);
        REQUIRE(match4.bestCandidate != nullptr);
        CHECK_FALSE(match4.isAmbiguous);
    }

    TEST_CASE("Tie and Identical Scores Trigger Ambiguity Flag")
    {
        SymbolTable table;
        Symbol symA;
        symA.name = "Test";
        symA.type = SymbolType::Function;
        FunctionSignature sigA;
        sigA.returnType = "void";
        sigA.parameters = {
            MakeParam("int", "a"),
            MakeParam("double", "b")
        };
        symA.signature = sigA;

        Symbol symB;
        symB.name = "Test";
        symB.type = SymbolType::Function;
        FunctionSignature sigB;
        sigB.returnType = "void";
        sigB.parameters = {
            MakeParam("double", "a"),
            MakeParam("int", "b")
        };
        symB.signature = sigB;

        std::vector<Symbol> candidates = { symA, symB };

        // Both require 1 widening conversion (int->double) -> equal score
        auto res = ResolveBestOverload(candidates, {"int", "int"}, table);
        CHECK(res.isAmbiguous);
        CHECK(res.viableCandidates.size() == 2);
    }

    TEST_CASE("Null Pointer Match to Handle vs Non-Handle")
    {
        SymbolTable table;
        Symbol symHandle;
        symHandle.name = "SetTarget";
        symHandle.type = SymbolType::Function;
        FunctionSignature sigH;
        sigH.returnType = "void";
        sigH.parameters = { MakeParam("Actor@", "target", true) };
        symHandle.signature = sigH;

        Symbol symVal;
        symVal.name = "SetTarget";
        symVal.type = SymbolType::Function;
        FunctionSignature sigV;
        sigV.returnType = "void";
        sigV.parameters = { MakeParam("int", "target") };
        symVal.signature = sigV;

        std::vector<Symbol> candidates = { symVal, symHandle };

        auto res = ResolveBestOverload(candidates, {"null"}, table);
        REQUIRE(res.bestCandidate != nullptr);
        CHECK(res.bestCandidate->GetFunction().parameters[0].typeName == "Actor@");
        CHECK(res.viableCandidates.size() == 1);
    }

    TEST_CASE("All Incompatible Candidates Return Nullptr")
    {
        SymbolTable table;
        Symbol sym;
        sym.name = "Process";
        sym.type = SymbolType::Function;
        FunctionSignature sig;
        sig.returnType = "void";
        sig.parameters = { MakeParam("string", "s") };
        sym.signature = sig;

        std::vector<Symbol> candidates = { sym };
        // Passing incompatible type (e.g. custom object or null)
        auto res = ResolveBestOverload(candidates, {"Actor@"}, table);
        CHECK(res.bestCandidate == nullptr);
        CHECK(res.viableCandidates.empty());
    }

    TEST_CASE("User Defined Conversion In Overload Resolution")
    {
        SymbolTable table;
        // opImplConv method: Fraction -> double
        Symbol convSym;
        convSym.name = "opImplConv";
        convSym.qualifiedName = "Fraction::opImplConv";
        convSym.type = SymbolType::Function;
        convSym.containerName = "Fraction";
        FunctionSignature convSig;
        convSig.returnType = "double";
        convSym.signature = convSig;
        table.AddSymbol(convSym);

        Symbol targetFunc;
        targetFunc.name = "Sqrt";
        targetFunc.type = SymbolType::Function;
        FunctionSignature targetSig;
        targetSig.returnType = "double";
        targetSig.parameters = { MakeParam("double", "val") };
        targetFunc.signature = targetSig;

        auto match = ResolveBestOverload({ targetFunc }, { "Fraction" }, table);
        REQUIRE(match.bestCandidate != nullptr);
        CHECK(match.bestScore == static_cast<int>(OverloadMatchPenalty::UserDefined));
    }
}

TEST_SUITE("Adversarial_DefiniteAssignment")
{
    TEST_CASE("Uninitialized Read Inside Ternary Expression")
    {
        std::string uninitTernary =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = true;\n"
            "    int y = cond ? x : 10;\n"
            "    Print(y);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(uninitTernary)));

        std::string uninitTernaryAlt =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool cond = true;\n"
            "    int y = cond ? 10 : x;\n"
            "    Print(y);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(uninitTernaryAlt)));
    }

    TEST_CASE("Uninitialized Read Inside Binary and Logical Operators")
    {
        std::string binaryRead =
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool b = (x > 5) && true;\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(binaryRead)));

        std::string compoundAssignRead =
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    x += 5;\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(compoundAssignRead)));
    }

    TEST_CASE("Complex Control Flow With Returns In Nested Branches")
    {
        // Branch 1 assigns, Branch 2 returns early -> post-if is definitely assigned
        std::string nestedReturnAssigned =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool a = true;\n"
            "    bool b = false;\n"
            "    if (a)\n"
            "    {\n"
            "        x = 10;\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "        if (b)\n"
            "        {\n"
            "            x = 20;\n"
            "        }\n"
            "        else\n"
            "        {\n"
            "            return;\n"
            "        }\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCodeForDiagnostics(nestedReturnAssigned)));

        // Branch 1 returns early, Branch 2 does not assign -> post-if is unassigned
        std::string nestedReturnUnassigned =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    bool a = true;\n"
            "    bool b = false;\n"
            "    if (a)\n"
            "    {\n"
            "        return;\n"
            "    }\n"
            "    else\n"
            "    {\n"
            "        if (b)\n"
            "        {\n"
            "            x = 20;\n"
            "        }\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(nestedReturnUnassigned)));
    }

    TEST_CASE("Unreachable Code After Return Does Not Emit False Warnings")
    {
        std::string unreachableCode =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    return;\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCodeForDiagnostics(unreachableCode)));
    }

    TEST_CASE("For Loop Without Initializer or Variable Reassignment")
    {
        std::string forLoopAssign =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    for (int i = 0; i < 10; ++i)\n"
            "    {\n"
            "        x = i;\n"
            "    }\n"
            "    Print(x);\n"
            "}\n";
        // A standard for loop with condition i < 10 might execute 0 times, so x is unassigned
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(forLoopAssign)));
    }

    TEST_CASE("Do While Guaranteed First Iteration Assigns")
    {
        std::string doWhileAssign =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    do {\n"
            "        x = 42;\n"
            "    } while (false);\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCodeForDiagnostics(doWhileAssign)));
    }

    TEST_CASE("If-Else Chain Full Coverage")
    {
        std::string ifElseChain =
            "void Print(int v) { }\n"
            "void main()\n"
            "{\n"
            "    int x;\n"
            "    int mode = 2;\n"
            "    if (mode == 1)\n"
            "        x = 10;\n"
            "    else if (mode == 2)\n"
            "        x = 20;\n"
            "    else\n"
            "        x = 30;\n"
            "    Print(x);\n"
            "}\n";
        CHECK_FALSE(HasUninitializedRead(AnalyzeCodeForDiagnostics(ifElseChain)));
    }

    TEST_CASE("Lambda and Nested Functions Are Analyzed")
    {
        std::string lambdaUninit =
            "void main()\n"
            "{\n"
            "    auto fn = function() {\n"
            "        int x;\n"
            "        return x + 1;\n"
            "    };\n"
            "}\n";
        CHECK(HasUninitializedRead(AnalyzeCodeForDiagnostics(lambdaUninit)));
    }
}
