#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "parser/AngelScriptParser.h"
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
    CHECK(sym.variableSignature.typeName == "int");
    CHECK(sym.variableSignature.modifiers.access == AccessModifier::Public);
    CHECK(sym.variableSignature.defaultValue.empty() == true);
}

TEST_CASE("SymbolCollector - Function Main and Local Variable Isolation Test")
{
    // Test Code: "void main() { int i = 0; }"
    // Expectation: Function 'main' is collected, but local variable 'i' inside function body is NOT collected in global SymbolTable
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
    CHECK(mainSym.functionSignature.returnType == "void");
    CHECK(mainSym.functionSignature.parameters.empty() == true);

    // 2. Verify local variable 'i' inside main() was NOT collected in global SymbolTable
    CHECK(symbolTable.HasSymbol("i") == false);
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
    CHECK(funcSym.functionSignature.returnType == "int &");
    CHECK(funcSym.functionSignature.modifiers.isReturnReference == true);
    CHECK(funcSym.functionSignature.modifiers.access == AccessModifier::Public);
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
    CHECK(sym.functionSignature.returnType == "float");
    CHECK(sym.functionSignature.modifiers.isShared == false);

    REQUIRE(sym.functionSignature.parameters.size() == 1);
    const auto &param = sym.functionSignature.parameters[0];
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
    CHECK(sym.functionSignature.returnType == "float");
    CHECK(sym.functionSignature.modifiers.isShared == true);

    REQUIRE(sym.functionSignature.parameters.size() == 2);

    // Parameter 1: radius
    const auto &p1 = sym.functionSignature.parameters[0];
    CHECK(p1.name == "radius");
    CHECK(p1.typeName == "const float");
    CHECK(p1.defaultValue == "0.0f");
    CHECK(p1.isConst == true);
    CHECK(p1.modifier == ParameterModifier::In);

    // Parameter 2: height
    const auto &p2 = sym.functionSignature.parameters[1];
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
    CHECK(sym.variableSignature.typeName == "constHandle@");
    CHECK(sym.variableSignature.modifiers.isHandle == true);
    CHECK(sym.variableSignature.modifiers.isConst == false);
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
    REQUIRE(sym.classSignature.bases.size() == 1);
    CHECK(sym.classSignature.bases[0] == "ClassBase");
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
    REQUIRE(sym.classSignature.bases.size() == 3);
    CHECK(sym.classSignature.bases[0] == "BaseClass");
    CHECK(sym.classSignature.bases[1] == "Interface1");
    CHECK(sym.classSignature.bases[2] == "Interface2");
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
