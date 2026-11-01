/**
 * @file MemoryLayoutAuditTest.cpp
 * @brief Memory layout audit asserting sizeof limits and quantifying savings for 100k instances.
 */

#include <doctest/doctest.h>

#include "analysis/NodeIndex.h"
#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"
#include "helpers/TestUtils.h"

using namespace angel_lsp::analysis;

static_assert(sizeof(TypeQualifiers) == 1, "TypeQualifiers must be exactly 1 byte");
static_assert(sizeof(SymbolModifiers) <= 4, "SymbolModifiers layout must not exceed 4 bytes");
static_assert(sizeof(ParameterInformation) <= 200, "ParameterInformation layout must not exceed 200 bytes");
static_assert(sizeof(VariableSignature) <= 176, "VariableSignature layout must not exceed 176 bytes");
static_assert(sizeof(FunctionSignature) <= 240, "FunctionSignature layout must not exceed 240 bytes");
static_assert(sizeof(FuncdefSignature) <= 152, "FuncdefSignature layout must not exceed 152 bytes");
static_assert(sizeof(ClassSignature) <= 80, "ClassSignature layout must not exceed 80 bytes");
static_assert(sizeof(LocalDefinition) <= 192, "LocalDefinition layout must not exceed 192 bytes");
static_assert(sizeof(Scope) <= 128, "Scope layout must not exceed 128 bytes");
static_assert(sizeof(IndexedContainerSpan) <= 20, "IndexedContainerSpan layout must not exceed 20 bytes");
static_assert(sizeof(NodeIndex) <= 248, "NodeIndex layout must not exceed 248 bytes");
static_assert(sizeof(Symbol) <= 504, "Symbol layout must not exceed 504 bytes");

TEST_SUITE("MemoryLayoutAudit")
{
    TEST_CASE("Hot structure sizes satisfy compact memory invariants")
    {
        CHECK(sizeof(TypeQualifiers) == 1);
        CHECK(sizeof(SymbolModifiers) <= 4);
        CHECK(sizeof(ParameterInformation) <= 200);
        CHECK(sizeof(VariableSignature) <= 176);
        CHECK(sizeof(FunctionSignature) <= 240);
        CHECK(sizeof(FuncdefSignature) <= 152);
        CHECK(sizeof(ClassSignature) <= 80);
        CHECK(sizeof(LocalDefinition) <= 192);
        CHECK(sizeof(Scope) <= 128);
        CHECK(sizeof(IndexedContainerSpan) <= 20);
        CHECK(sizeof(NodeIndex) <= 248);
        CHECK(sizeof(Symbol) <= 504);
    }

    TEST_CASE("Quantify memory savings for 100,000 instances")
    {
        // Baseline sizes before compaction (bytes on x64):
        constexpr size_t k_preModifiers = 24;
        constexpr size_t k_preParamInfo = 248;
        constexpr size_t k_preVarSig = 216;
        constexpr size_t k_preFuncSig = 264;
        constexpr size_t k_preFuncdefSig = 176;
        constexpr size_t k_preClassSig = 104;
        constexpr size_t k_preLocalDef = 208;
        constexpr size_t k_preScope = 128;
        constexpr size_t k_preSpan = 24;
        constexpr size_t k_preNodeIndex = 248;
        constexpr size_t k_preSymbol = 528;

        constexpr size_t k_savedPerSet =
            (k_preModifiers - sizeof(SymbolModifiers)) + (k_preParamInfo - sizeof(ParameterInformation)) +
            (k_preVarSig - sizeof(VariableSignature)) + (k_preFuncSig - sizeof(FunctionSignature)) +
            (k_preFuncdefSig - sizeof(FuncdefSignature)) + (k_preClassSig - sizeof(ClassSignature)) +
            (k_preLocalDef - sizeof(LocalDefinition)) + (k_preScope - sizeof(Scope)) +
            (k_preSpan - sizeof(IndexedContainerSpan)) + (k_preNodeIndex - sizeof(NodeIndex)) +
            (k_preSymbol - sizeof(Symbol));

        CHECK(k_savedPerSet >= 250);

        constexpr size_t k_instances = 100000;
        const size_t bytesSavedFor100k = k_savedPerSet * k_instances;
        const double mbSavedFor100k = static_cast<double>(bytesSavedFor100k) / (1024.0 * 1024.0);

        // Savings must exceed 20 MB for 100k full instance sets
        CHECK(mbSavedFor100k >= 20.0);
    }

    TEST_CASE("Randomized fixture verification for compacted structures")
    {
        const std::string randomName = angel_lsp::test::GenerateRandomSymbolName("func");
        const std::string randomType = angel_lsp::test::GenerateRandomSymbolName("Type");

        ParameterInformation param;
        param.name = randomName;
        param.typeName = randomType;
        param.isConst = true;
        param.isHandle = true;
        param.isReference = false;
        param.typeKind = TypeKind::Object;
        param.modifier = ParameterModifier::In;

        CHECK(param.name == randomName);
        CHECK(param.typeName == randomType);
        CHECK(param.isConst);
        CHECK(param.isHandle);
        CHECK_FALSE(param.isReference);
        CHECK(param.typeKind == TypeKind::Object);
        CHECK(param.modifier == ParameterModifier::In);

        SymbolModifiers modifiers;
        modifiers.isConst = true;
        modifiers.isExplicit = false;
        modifiers.isFinal = true;
        CHECK(modifiers.isConst);
        CHECK_FALSE(modifiers.isExplicit);
        CHECK(modifiers.isFinal);
    }
}
