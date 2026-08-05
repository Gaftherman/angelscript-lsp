#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
#include <tree_sitter/api.h>
#include "utils/LspLogger.h"

using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_CASE("SymbolCollector - Global Variable Test")
{
    // Test Code: "int property;"
    std::string sourceCode = "int property;";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    // 1. Verify symbol presence
    CHECK(symbolTable.HasSymbol("property") == true);

    auto symbols = symbolTable.FindSymbols("property");
    REQUIRE(symbols.size() == 1);

    // 2. Verify signature details
    const auto &sym = symbols[0];
    CHECK(sym.type == SymbolType::Variable);
    CHECK(sym.name == "property");
    CHECK(sym.GetVariable().typeName == "int");
    CHECK(sym.GetVariable().modifiers.access == AccessModifier::Public);
    CHECK(sym.GetVariable().defaultValue.empty() == true);
}

TEST_CASE("SymbolCollector - Function Main and Local Variable Symbol Indexing Test")
{
    // Test Code: "void main() { int i = 0; }"
    // Expectation: Function 'main' is collected, and local variable 'i' inside function body is collected with containerName 'main'
    std::string sourceCode = "void main()\n{\n    int i = 0;\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    // 1. Verify Function 'main' is present
    CHECK(symbolTable.HasSymbol("main") == true);

    auto mainSymbols = symbolTable.FindSymbols("main");
    REQUIRE(mainSymbols.size() == 1);

    const auto &mainSym = mainSymbols[0];
    CHECK(mainSym.type == SymbolType::Function);
    CHECK(mainSym.name == "main");
    CHECK(mainSym.GetFunction().returnType == "void");
    CHECK(mainSym.GetFunction().parameters.empty() == true);

    // 2. Verify local variable 'i' inside main() was collected with containerName 'main'
    CHECK(symbolTable.HasSymbolAnywhere("i") == true);
    auto iSymbols = symbolTable.FindSymbols("main::i");
    REQUIRE(iSymbols.size() == 1);
    CHECK(iSymbols[0].containerName == "main");
}

TEST_CASE("SymbolCollector - Reference Return Function Test")
{
    // Test Code: "int &Function() { return property; }"
    std::string sourceCode = "int &Function()\n{\n    return property;\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    // 1. Verify Function 'Function' is present
    CHECK(symbolTable.HasSymbol("Function") == true);

    auto funcSymbols = symbolTable.FindSymbols("Function");
    REQUIRE(funcSymbols.size() == 1);

    // 2. Verify return reference flag and return type
    const auto &funcSym = funcSymbols[0];
    CHECK(funcSym.type == SymbolType::Function);
    CHECK(funcSym.name == "Function");
    CHECK(funcSym.GetFunction().returnType == "int &");
    CHECK(funcSym.GetFunction().modifiers.isReturnReference == true);
    CHECK(funcSym.GetFunction().modifiers.access == AccessModifier::Public);
}

TEST_CASE("SymbolCollector - Function With Basic Default Parameter")
{
    // Test Code: "float calcularArea(float radius = 0.0f)"
    std::string sourceCode = "float calcularArea(float radius = 0.0f)\n{\n    return 3.14 * radius * radius;\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    CHECK(symbolTable.HasSymbol("calcularArea") == true);

    auto funcSymbols = symbolTable.FindSymbols("calcularArea");
    REQUIRE(funcSymbols.size() == 1);

    const auto &sym = funcSymbols[0];
    CHECK(sym.type == SymbolType::Function);
    CHECK(sym.name == "calcularArea");
    CHECK(sym.GetFunction().returnType == "float");
    CHECK(sym.GetFunction().modifiers.isShared == false);

    REQUIRE(sym.GetFunction().parameters.size() == 1);
    const auto &param = sym.GetFunction().parameters[0];
    CHECK(param.name == "radius");
    CHECK(param.typeName == "float");
    CHECK(param.defaultValue == "0.0f");
    CHECK(param.isConst == false);
    CHECK(param.modifier == ParameterModifier::None);
}

TEST_CASE("SymbolCollector - Complex Function With Shared and Multiple Reference Parameters")
{
    // Test Code: "shared float calcularArea(const float &in radius = 0.0f, const float &in height = 0.0f)"
    std::string sourceCode = "shared float calcularArea(const float &in radius = 0.0f, const float &in height = 0.0f)\n{\n    return 3.14 * radius * radius;\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    CHECK(symbolTable.HasSymbol("calcularArea") == true);

    auto funcSymbols = symbolTable.FindSymbols("calcularArea");
    REQUIRE(funcSymbols.size() == 1);

    const auto &sym = funcSymbols[0];
    CHECK(sym.type == SymbolType::Function);
    CHECK(sym.name == "calcularArea");
    CHECK(sym.GetFunction().returnType == "float");
    CHECK(sym.GetFunction().modifiers.isShared == true);

    REQUIRE(sym.GetFunction().parameters.size() == 2);

    // Parameter 1: radius
    const auto &p1 = sym.GetFunction().parameters[0];
    CHECK(p1.name == "radius");
    CHECK(p1.typeName == "const float");
    CHECK(p1.defaultValue == "0.0f");
    CHECK(p1.isConst == true);
    CHECK(p1.modifier == ParameterModifier::In);

    // Parameter 2: height
    const auto &p2 = sym.GetFunction().parameters[1];
    CHECK(p2.name == "height");
    CHECK(p2.typeName == "const float");
    CHECK(p2.defaultValue == "0.0f");
    CHECK(p2.isConst == true);
    CHECK(p2.modifier == ParameterModifier::In);
}

TEST_CASE("SymbolCollector - No False Positive for constHandle Type Name")
{
    // Test Code: "constHandle@ constVar;"
    // Expectation: Type is "constHandle@", isHandle is true, but isConst MUST BE FALSE
    std::string sourceCode = "constHandle@ constVar;";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    CHECK(symbolTable.HasSymbol("constVar") == true);

    auto symbols = symbolTable.FindSymbols("constVar");
    REQUIRE(symbols.size() == 1);

    const auto &sym = symbols[0];
    CHECK(sym.type == SymbolType::Variable);
    CHECK(sym.name == "constVar");
    CHECK(sym.GetVariable().typeName == "constHandle@");
    CHECK(sym.GetVariable().modifiers.isHandle == true);
    CHECK(sym.GetVariable().modifiers.isConst == false);
}

TEST_CASE("SymbolCollector - Class Inheritance Extraction Test")
{
    // Test Code: "class ClassHereny : ClassBase"
    // Expectation: Symbol 'ClassHereny' has baseClass == "ClassBase"
    std::string sourceCode = "class ClassHereny : ClassBase\n{\n    void MyMethod() {}\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    CHECK(symbolTable.HasSymbol("ClassHereny") == true);

    auto classSymbols = symbolTable.FindSymbols("ClassHereny");
    REQUIRE(classSymbols.size() == 1);

    const auto &sym = classSymbols[0];
    CHECK(sym.type == SymbolType::Class);
    CHECK(sym.name == "ClassHereny");
    REQUIRE(sym.GetClass().bases.size() == 1);
    CHECK(sym.GetClass().bases[0] == "ClassBase");
}

TEST_CASE("SymbolCollector - Multi-Inheritance Class Extraction Test")
{
    // Test Code: "class MultyHerentClass : BaseClass, Interface1, Interface2 {}"
    std::string sourceCode = "class MultyHerentClass : BaseClass, Interface1, Interface2\n{\n}";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    CHECK(symbolTable.HasSymbol("MultyHerentClass") == true);

    auto classSymbols = symbolTable.FindSymbols("MultyHerentClass");
    REQUIRE(classSymbols.size() == 1);

    const auto &sym = classSymbols[0];
    CHECK(sym.type == SymbolType::Class);
    CHECK(sym.name == "MultyHerentClass");
    REQUIRE(sym.GetClass().bases.size() == 3);
    CHECK(sym.GetClass().bases[0] == "BaseClass");
    CHECK(sym.GetClass().bases[1] == "Interface1");
    CHECK(sym.GetClass().bases[2] == "Interface2");
}

TEST_CASE("SymbolCollector - Namespace Scope and Symbol Qualified Names Test")
{
    std::string sourceCode = R"(
namespace MyNamespace
{
    int myVariable;

    int MyFunction()
    {
        return 42;
    }

    int AnotherFunction()
    {
        return myVariable;
    }

    int OverloadedFunction(int x)
    {
        return x * 2;
    }

    float OverloadedFunction(float x)
    {
        return x / 2.0;
    }

    string MultipleOverloads(string x, int y)
    {
        return "String overload: " + x + ", " + y;
    }
}
)";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable);

    // 1. Verify Namespace symbol
    CHECK(symbolTable.HasSymbol("MyNamespace") == true);

    // 2. Verify Member variable 'myVariable' containerName and qualifiedName
    CHECK(symbolTable.HasSymbol("MyNamespace::myVariable") == true);
    auto varSyms = symbolTable.FindSymbols("MyNamespace::myVariable");
    REQUIRE(varSyms.size() == 1);
    CHECK(varSyms[0].name == "myVariable");
    CHECK(varSyms[0].containerName == "MyNamespace");
    CHECK(varSyms[0].qualifiedName == "MyNamespace::myVariable");

    // 3. Verify Function 'MyFunction' containerName and qualifiedName
    CHECK(symbolTable.HasSymbol("MyNamespace::MyFunction") == true);
    auto funcSyms = symbolTable.FindSymbols("MyNamespace::MyFunction");
    REQUIRE(funcSyms.size() == 1);
    CHECK(funcSyms[0].name == "MyFunction");
    CHECK(funcSyms[0].containerName == "MyNamespace");
    CHECK(funcSyms[0].qualifiedName == "MyNamespace::MyFunction");

    // 4. Verify Overloaded Function 'OverloadedFunction'
    CHECK(symbolTable.HasSymbol("MyNamespace::OverloadedFunction") == true);
    auto overloads = symbolTable.FindSymbols("MyNamespace::OverloadedFunction");
    REQUIRE(overloads.size() == 2);
    CHECK(overloads[0].containerName == "MyNamespace");
    CHECK(overloads[1].containerName == "MyNamespace");
}

TEST_CASE("SymbolCollector - Syntax Error i18n and Token Formatting Test")
{
    std::string sourceCode = "class SomeClass {";
    std::string fileUri = "file:///test.as";

    SymbolTable symbolTable;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    SUBCASE("English Syntax Error Token Formatting")
    {
        angel_lsp::i18n::I18n i18nEn("en");
        auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable, &i18nEn);
        REQUIRE(diagnostics.size() >= 1);
        CHECK(diagnostics[0].code == "as-syntax-error");
        CHECK(diagnostics[0].message == "Syntax error: missing '}'");
    }

    SUBCASE("Spanish Syntax Error Token Formatting")
    {
        angel_lsp::i18n::I18n i18nEs("es");
        auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, symbolTable, &i18nEs);
        REQUIRE(diagnostics.size() >= 1);
        CHECK(diagnostics[0].code == "as-syntax-error");
        CHECK(diagnostics[0].message == "Error de sintaxis: falta '}'");
    }
}

TEST_CASE("SymbolCollector - TypeInfo extraction from TSNode")
{
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);

    auto collect = [&](const std::string &code) -> Symbol
    {
        SymbolTable table;
        collector.CollectSymbols("file:///test.as", code, parser, table);
        auto syms = table.FindSymbols("x");
        REQUIRE(syms.size() == 1);
        return syms[0];
    };

    SUBCASE("int x")
    {
        auto sym = collect("int x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Int32);
        CHECK(sym.GetVariable().isArray == false);
        CHECK(sym.GetVariable().arrayDepth == 0);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("int[] x")
    {
        auto sym = collect("int[] x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 1);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("int[][] x")
    {
        auto sym = collect("int[][] x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 2);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("int[]@ x")
    {
        auto sym = collect("int[]@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 1);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("int[]@[]@ x")
    {
        auto sym = collect("int[]@[]@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 2);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("array<int> x")
    {
        auto sym = collect("array<int> x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 1);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("array<int>@ x")
    {
        auto sym = collect("array<int>@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 1);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("array<array<int>> x")
    {
        auto sym = collect("array<array<int>> x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 2);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("array<array<int>@>@ x")
    {
        auto sym = collect("array<array<int>@>@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 2);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "int");
    }

    SUBCASE("array<Player@> x")
    {
        auto sym = collect("array<Player@> x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 1);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "Player");
    }

    SUBCASE("Player@ x")
    {
        auto sym = collect("Player@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Handle);
        CHECK(sym.GetVariable().isArray == false);
        CHECK(sym.GetVariable().arrayDepth == 0);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "Player");
    }

    SUBCASE("ObjectHandle@[]@[]@ x")
    {
        auto sym = collect("ObjectHandle@[]@[]@ x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Array);
        CHECK(sym.GetVariable().isArray == true);
        CHECK(sym.GetVariable().arrayDepth == 2);
        CHECK(sym.GetVariable().modifiers.isHandle == true);
        CHECK(sym.GetVariable().baseTypeName == "ObjectHandle");
    }

    SUBCASE("Player x")
    {
        auto sym = collect("Player x;");
        CHECK(sym.GetVariable().typeKind == TypeKind::Unknown);
        CHECK(sym.GetVariable().isArray == false);
        CHECK(sym.GetVariable().arrayDepth == 0);
        CHECK(sym.GetVariable().modifiers.isHandle == false);
        CHECK(sym.GetVariable().baseTypeName == "Player");
    }
}

TEST_CASE("SymbolCollector - Parse Error on Multiple Consecutive Colons")
{
    std::string sourceCode = ":::::::::::::::::::::::var::::name varname;";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    REQUIRE(diagnostics.size() >= 1);
    CHECK(diagnostics[0].code == "as-syntax-error");
}

TEST_CASE("SymbolCollector - Using Namespace Reserved Keyword")
{
    std::string sourceCode = "using namespace class;\nusing namespace int;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-reserved-keyword-name");
    CHECK(diagnostics[1].code == "as-err-reserved-keyword-name");
}

TEST_CASE("SymbolCollector - Using Namespace Valid Identifier No Diagnostic")
{
    std::string sourceCode = "using namespace Foo;\nusing namespace Foo::Bar;\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    REQUIRE(diagnostics.empty());
}

TEST_CASE("SymbolCollector - Using Namespace Reserved Keyword Inside Namespace")
{
    std::string sourceCode = "namespace NS { using namespace int; }\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics[0].code == "as-err-reserved-keyword-name");
}

TEST_CASE("SymbolCollector - Duplicate Declaration Modifier Warning")
{
    std::string sourceCode = "final final class Foo {}\nshared shared interface Bar {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    auto diagnostics = collector.CollectSymbols(fileUri, sourceCode, parser, table);

    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics[0].code == "as-err-attribute-repeated");
    CHECK(diagnostics[0].severity == DiagnosticSeverity::Warning);
    CHECK(diagnostics[1].code == "as-err-attribute-repeated");
    CHECK(diagnostics[1].severity == DiagnosticSeverity::Warning);
}

TEST_CASE("SymbolCollector - Goto Statement AST Extraction")
{
    std::string sourceCode = "void test() {\n    goto my_target;\nmy_target:\n}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbol("test") == true);
    auto syms = table.FindSymbols("test");
    REQUIRE(syms.size() == 1);

    const auto &fnSig = syms[0].GetFunction();
    REQUIRE(fnSig.bodyAnalysis.has_value());
    REQUIRE(fnSig.bodyAnalysis->gotoTargetLabels.size() == 1);
    CHECK(fnSig.bodyAnalysis->gotoTargetLabels[0] == "my_target");
}

TEST_CASE("SymbolCollector - Syntax Error Recovery Test")
{
    // Source code with broken syntax before a valid function
    std::string sourceCode = "class BrokenClass {\n  int invalid syntax here\n}\nvoid validFunction() {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    // Verify validFunction was recovered and collected despite syntax error in BrokenClass
    CHECK(table.HasSymbol("validFunction") == true);
    auto syms = table.FindSymbols("validFunction");
    REQUIRE(syms.size() == 1);
    CHECK(syms[0].type == SymbolType::Function);
}

TEST_CASE("SymbolCollector - Exact Position Accuracy Test")
{
    std::string sourceCode = "class Foo {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbol("Foo") == true);
    auto syms = table.FindSymbols("Foo");
    REQUIRE(syms.size() == 1);

    const auto &sym = syms[0];
    CHECK(sym.startLine == 0);
    CHECK(sym.startCharacter == 0);
    CHECK(sym.endLine == 0);
    CHECK(sym.endCharacter == 12);
}

TEST_CASE("SymbolCollector - Mixin Name False Positive Edge Case")
{
    std::string sourceCode = "class MixinHelper {}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbol("MixinHelper") == true);
    auto syms = table.FindSymbols("MixinHelper");
    REQUIRE(syms.size() == 1);

    const auto &classSig = syms[0].GetClass();
    CHECK(classSig.modifiers.isMixin == false);
}

TEST_CASE("SymbolCollector - Goto In String Literal False Positive Edge Case")
{
    std::string sourceCode = "void foo() {\n    string s = \"goto fake_label;\";\n}\n";
    std::string fileUri = "file:///test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbol("foo") == true);
    auto syms = table.FindSymbols("foo");
    REQUIRE(syms.size() == 1);

    const auto &fnSig = syms[0].GetFunction();
    CHECK(fnSig.bodyAnalysis.has_value());
    CHECK(fnSig.bodyAnalysis->gotoTargetLabels.empty() == true);
}

TEST_CASE("SymbolCollector - Import Declaration Indexing")
{
    std::string sourceCode = "import void ExternalFunction(int a, float b) from \"MyModule\";\n";
    std::string fileUri = "file:///import_test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbol("ExternalFunction") == true);
    auto syms = table.FindSymbols("ExternalFunction");
    REQUIRE(syms.size() == 1);

    const auto &sym = syms[0];
    CHECK(sym.type == SymbolType::Function);
    CHECK(sym.name == "ExternalFunction");
    CHECK(sym.GetFunction().modifiers.isExternal == true);
    CHECK(sym.GetFunction().returnType == "void");
    REQUIRE(sym.GetFunction().parameters.size() == 2);
    CHECK(sym.GetFunction().parameters[0].name == "a");
    CHECK(sym.GetFunction().parameters[1].name == "b");
}

TEST_CASE("SymbolCollector - Local Variable Symbol Indexing & Ranges")
{
    std::string sourceCode = "void process() {\n    int localVar = 42;\n}\n";
    std::string fileUri = "file:///local_test.as";

    SymbolTable table;
    AngelScriptParser parser;
    SymbolCollector collector(nullptr);
    collector.CollectSymbols(fileUri, sourceCode, parser, table);

    CHECK(table.HasSymbolAnywhere("localVar") == true);
    auto syms = table.FindSymbols("process::localVar");
    REQUIRE(syms.size() == 1);

    const auto &sym = syms[0];
    CHECK(sym.type == SymbolType::Variable);
    CHECK(sym.name == "localVar");
    CHECK(sym.containerName == "process");
    CHECK(sym.startLine == 1);
    CHECK(sym.GetVariable().typeName == "int");
    CHECK(sym.GetVariable().defaultValue == "42");
}


