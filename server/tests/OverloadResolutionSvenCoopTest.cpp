/**
 * @file OverloadResolutionSvenCoopTest.cpp
 * @brief Unit tests for real-world Sven Co-op overload resolution patterns.
 */

#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolTable.h"
#include "analysis/TypeConversionChecker.h"
#include "helpers/TestUtils.h"
#include "parser/AngelScriptParser.h"

#include <doctest/doctest.h>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::parser;

TEST_SUITE("OverloadResolutionSvenCoop")
{
    /**
     * @brief Verifies that R-values prefer by-value or const reference over &out and &inout.
     */
    TEST_CASE("OverloadResolver - R-value prefers const ref over out parameter")
    {
        const std::string funcName = test::GenerateRandomSymbolName("LogVal");
        SymbolTable table;

        // Overload 1: void LogVal(int& out)
        FunctionSignature sigOut;
        sigOut.returnType = "void";
        ParameterInformation pOut;
        pOut.name = "val";
        pOut.typeName = "int";
        pOut.modifier = ParameterModifier::Out;
        pOut.isReference = true;
        sigOut.parameters.push_back(pOut);

        Symbol symOut;
        symOut.name = funcName;
        symOut.type = SymbolType::Function;
        symOut.signature = sigOut;

        // Overload 2: void LogVal(const int& in)
        FunctionSignature sigConst;
        sigConst.returnType = "void";
        ParameterInformation pConst;
        pConst.name = "val";
        pConst.typeName = "int";
        pConst.modifier = ParameterModifier::In;
        pConst.isConst = true;
        pConst.isReference = true;
        sigConst.parameters.push_back(pConst);

        Symbol symConst;
        symConst.name = funcName;
        symConst.type = SymbolType::Function;
        symConst.signature = sigConst;

        const std::vector<Symbol> candidates = {symOut, symConst};

        // R-value argument (isLValue = false)
        auto result = ResolveBestOverload(candidates, {"int"}, table, {false});
        REQUIRE(result.bestCandidate != nullptr);
        CHECK(result.bestCandidate->GetFunction().parameters[0].modifier == ParameterModifier::In);

        // L-value argument (isLValue = true)
        auto resultLVal = ResolveBestOverload(candidates, {"int"}, table, {true});
        REQUIRE(resultLVal.bestCandidate != nullptr);
    }

    /**
     * @brief Verifies container disambiguation between different template argument types.
     */
    TEST_CASE("OverloadResolver - Disambiguates between array<string> and array<float>")
    {
        const std::string funcName = test::GenerateRandomSymbolName("ProcessList");
        SymbolTable table;

        // Overload 1: void ProcessList(array<string>@)
        FunctionSignature sigStr;
        sigStr.returnType = "void";
        ParameterInformation pStr;
        pStr.name = "items";
        pStr.typeName = "array<string>@";
        pStr.isHandle = true;
        sigStr.parameters.push_back(pStr);

        Symbol symStr;
        symStr.name = funcName;
        symStr.type = SymbolType::Function;
        symStr.signature = sigStr;

        // Overload 2: void ProcessList(array<float>@)
        FunctionSignature sigFlt;
        sigFlt.returnType = "void";
        ParameterInformation pFlt;
        pFlt.name = "items";
        pFlt.typeName = "array<float>@";
        pFlt.isHandle = true;
        sigFlt.parameters.push_back(pFlt);

        Symbol symFlt;
        symFlt.name = funcName;
        symFlt.type = SymbolType::Function;
        symFlt.signature = sigFlt;

        const std::vector<Symbol> candidates = {symStr, symFlt};

        CHECK(AreIncompatibleTemplateTypes("array<string>", "array<float>"));
        CHECK(AreIncompatibleTemplateTypes("array<string>@", "array<float>@"));
        CHECK_FALSE(AreIncompatibleTemplateTypes("array<string>", "array<string>"));

        auto matchStr = ResolveBestOverload(candidates, {"array<string>@"}, table);
        REQUIRE(matchStr.bestCandidate != nullptr);
        CHECK(matchStr.bestCandidate->GetFunction().parameters[0].typeName == "array<string>@");

        auto matchFlt = ResolveBestOverload(candidates, {"array<float>@"}, table);
        REQUIRE(matchFlt.bestCandidate != nullptr);
        CHECK(matchFlt.bestCandidate->GetFunction().parameters[0].typeName == "array<float>@");
    }

    /**
     * @brief Verifies that default values allow flexible overload selection.
     */
    TEST_CASE("OverloadResolver - Dispatches correctly when optional arguments have defaults")
    {
        const std::string funcName = test::GenerateRandomSymbolName("DispatchEvent");
        SymbolTable table;

        // void DispatchEvent(int code, float duration = 1.0f, bool broadcast = true)
        FunctionSignature sig;
        sig.returnType = "void";

        ParameterInformation p1;
        p1.name = "code";
        p1.typeName = "int";
        sig.parameters.push_back(p1);

        ParameterInformation p2;
        p2.name = "duration";
        p2.typeName = "float";
        p2.defaultValue = "1.0f";
        sig.parameters.push_back(p2);

        ParameterInformation p3;
        p3.name = "broadcast";
        p3.typeName = "bool";
        p3.defaultValue = "true";
        sig.parameters.push_back(p3);

        Symbol sym;
        sym.name = funcName;
        sym.type = SymbolType::Function;
        sym.signature = sig;

        const std::vector<Symbol> candidates = {sym};

        auto match1 = ResolveBestOverload(candidates, {"int"}, table);
        REQUIRE(match1.bestCandidate != nullptr);

        auto match2 = ResolveBestOverload(candidates, {"int", "float"}, table);
        REQUIRE(match2.bestCandidate != nullptr);

        auto match3 = ResolveBestOverload(candidates, {"int", "float", "bool"}, table);
        REQUIRE(match3.bestCandidate != nullptr);
    }
}
