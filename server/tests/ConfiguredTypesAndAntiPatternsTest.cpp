/**
 * @file ConfiguredTypesAndAntiPatternsTest.cpp
 * @brief Invariant-based tests verifying elimination of hardcoded type antipatterns:
 *        'any' vs '?' wildcard isolation, dynamic container parameter resolution via SymbolTable,
 *        custom array desugaring, and request effective type getters.
 */

#include "analysis/DiagnosticCodes.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticAnalysisRequest.h"
#include "analysis/SymbolTable.h"
#include "analysis/overload/ConversionRankingEngine.h"
#include "analysis/overload/OverloadTypeConversions.h"
#include "config/ServerConfig.h"
#include "helpers/TestUtils.h"

#include <doctest/doctest.h>
#include <ostream>
#include <string>

using namespace angel_lsp;
using namespace angel_lsp::analysis;
using namespace angel_lsp::test;

TEST_SUITE("ConfiguredTypesAndAntiPatterns")
{
    TEST_CASE("Wildcard Parameter Invariant: strictly '?' and not 'any' add-on class")
    {
        const std::string randomTypeName = GenerateRandomSymbolName("CustomType");

        ParameterInformation wildcardParam;
        wildcardParam.typeName = "?";
        CHECK(IsWildcardParameter(wildcardParam));

        ParameterInformation wildcardRefParam;
        wildcardRefParam.typeName = "?&in";
        wildcardRefParam.rawText = "const ?&in";
        CHECK(IsWildcardParameter(wildcardRefParam));

        ParameterInformation anyParam;
        anyParam.typeName = "any";
        anyParam.rawText = "any";
        CHECK_FALSE(IsWildcardParameter(anyParam));

        ParameterInformation anyHandleParam;
        anyHandleParam.typeName = "any@";
        anyHandleParam.rawText = "any@";
        CHECK_FALSE(IsWildcardParameter(anyHandleParam));

        ParameterInformation regularParam;
        regularParam.typeName = randomTypeName;
        regularParam.rawText = randomTypeName;
        CHECK_FALSE(IsWildcardParameter(regularParam));
    }

    TEST_CASE("Container Parameter Invariant: native bracket syntax and dynamic template resolution")
    {
        const std::string elemType = GenerateRandomSymbolName("Elem");
        const std::string customArrayName = GenerateRandomSymbolName("ArrayContainer");
        const std::string customTemplateName = GenerateRandomSymbolName("optional");

        // 1. Native bracket syntax
        ParameterInformation bracketParam;
        bracketParam.typeName = elemType + "[]";
        bracketParam.rawText = elemType + "[]";
        CHECK(IsContainerParameter(bracketParam));

        // 2. Default array template
        ParameterInformation defaultArrayParam;
        defaultArrayParam.typeName = "array<" + elemType + ">";
        defaultArrayParam.rawText = "array<" + elemType + ">";
        CHECK(IsContainerParameter(defaultArrayParam));

        // 3. Configured array container name
        ParameterInformation customArrayParam;
        customArrayParam.typeName = customArrayName + "<" + elemType + ">";
        customArrayParam.rawText = customArrayName + "<" + elemType + ">";
        CHECK(IsContainerParameter(customArrayParam, nullptr, customArrayName));

        // 4. vector< is not a built-in container in AngelScript and must not be accepted unless declared
        SymbolTable emptyTable;
        ParameterInformation vectorParam;
        vectorParam.typeName = "vector<" + elemType + ">";
        vectorParam.rawText = "vector<" + elemType + ">";
        CHECK_FALSE(IsContainerParameter(vectorParam, &emptyTable, "array"));

        // 5. Dynamic SymbolTable lookup: custom template class (e.g. optional<T> from .as.predefined)
        SymbolTable tableWithTemplate;
        Symbol templateSym;
        templateSym.type = SymbolType::Class;
        templateSym.name = customTemplateName;
        ClassSignature clsSig;
        clsSig.isTemplate = true;
        clsSig.templateParams.push_back("T");
        templateSym.signature = clsSig;
        tableWithTemplate.AddSymbol(templateSym);

        ParameterInformation dynamicTemplateParam;
        dynamicTemplateParam.typeName = customTemplateName + "<" + elemType + ">";
        dynamicTemplateParam.rawText = customTemplateName + "<" + elemType + ">";
        CHECK(IsContainerParameter(dynamicTemplateParam, &tableWithTemplate, "array"));
    }

    TEST_CASE("Desugar Array Brackets Invariant: respects configured array type name")
    {
        const std::string elemType = GenerateRandomSymbolName("DataType");
        const std::string customArray = GenerateRandomSymbolName("ArrayT");

        // Default "array"
        const std::string singleDesugar = DesugarArrayBrackets(elemType + "[]");
        CHECK(singleDesugar == "array<" + elemType + ">");

        // Custom array container
        const std::string customDesugar = DesugarArrayBrackets(elemType + "[]", customArray);
        CHECK(customDesugar == customArray + "<" + elemType + ">");

        // Multidimensional array with custom container
        const std::string multiDesugar = DesugarArrayBrackets(elemType + "[][]", customArray);
        CHECK(multiDesugar == customArray + "<" + customArray + "<" + elemType + ">>");
    }

    TEST_CASE("SemanticAnalysisRequest Invariant: effective getters guarantee non-empty defaults")
    {
        const std::string customStr = GenerateRandomSymbolName("CustomString");
        const std::string customArr = GenerateRandomSymbolName("CustomArray");

        SymbolTable table;
        // 1. Unconfigured request (typeConfig == nullptr)
        SemanticAnalysisRequest defaultReq{table, "file:///test.as", ".as.predefined"};
        CHECK(defaultReq.GetStringTypeName().empty());
        CHECK(defaultReq.GetArrayTypeName().empty());
        CHECK(defaultReq.GetEffectiveStringTypeName() == "string");
        CHECK(defaultReq.GetEffectiveArrayTypeName() == "array");

        // 2. Configured request
        config::TypeConfig typeCfg;
        typeCfg.stringTypeName = customStr;
        typeCfg.arrayTypeName = customArr;

        SemanticAnalysisRequest configuredReq{table, "file:///test.as", ".as.predefined"};
        configuredReq.typeConfig = &typeCfg;
        CHECK(configuredReq.GetStringTypeName() == customStr);
        CHECK(configuredReq.GetArrayTypeName() == customArr);
        CHECK(configuredReq.GetEffectiveStringTypeName() == customStr);
        CHECK(configuredReq.GetEffectiveArrayTypeName() == customArr);
    }

    TEST_CASE("Overload Conversion Invariant: 'any' does not act as wildcard and dynamic container matches init_list")
    {
        const std::string elemType = GenerateRandomSymbolName("Item");
        const std::string templateName = GenerateRandomSymbolName("optional");

        SymbolTable table;
        Symbol templateSym;
        templateSym.type = SymbolType::Class;
        templateSym.name = templateName;
        ClassSignature clsSig;
        clsSig.isTemplate = true;
        clsSig.templateParams.push_back("T");
        templateSym.signature = clsSig;
        table.AddSymbol(templateSym);

        ParameterInformation anyParam;
        anyParam.typeName = "any";
        anyParam.name = "a";

        // Passing "int" to an "any" parameter should NOT be Exact wildcard match
        const auto anyConv = EvaluateArgumentConversion("int", anyParam, table);
        CHECK_FALSE(anyConv.rank == ConversionRank::Exact);

        // Passing "init_list" to a dynamically declared template container should match as Exact
        ParameterInformation containerParam;
        containerParam.typeName = templateName + "<" + elemType + ">";
        containerParam.name = "c";
        const auto containerConv = EvaluateArgumentConversion("init_list", containerParam, table);
        CHECK(containerConv.rank == ConversionRank::Exact);
    }
}
