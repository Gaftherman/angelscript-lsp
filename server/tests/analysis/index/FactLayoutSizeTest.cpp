#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/index/LValueSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/Symbol.h"

#include <doctest/doctest.h>

using namespace angel_lsp::analysis;

TEST_SUITE("Fact layout")
{
    TEST_CASE("Indexed fact structs stay within their size budget")
    {
        // CallSiteFacts.h
        CHECK(sizeof(CallArgumentTernaryFact) <= 16);
        CHECK(sizeof(CallLambdaParamFact) <= 12);
        CHECK(sizeof(CallArgumentFact) <= 48);
        CHECK(sizeof(CallSiteFact) <= 108);
        CHECK(sizeof(DirectInitSiteFact) <= 56);
        // ExpressionSiteFacts.h
        CHECK(sizeof(BinarySiteFact) <= 8);
        CHECK(sizeof(FoldedConstantFact) <= 24);
        CHECK(sizeof(UnaryPostfixSiteFact) <= 24);
        CHECK(sizeof(MemberExpressionSiteFact) <= 16);
        CHECK(sizeof(IndexExpressionSiteFact) <= 28);
        CHECK(sizeof(CastExpressionSiteFact) <= 24);
        // LValueSiteFacts.h
        CHECK(sizeof(LValueBranchFact) <= 12);
        CHECK(sizeof(LValueMemberCandidateFact) <= 16);
        CHECK(sizeof(LValueCallSiteFact) <= 52);
        CHECK(sizeof(LValueCandidateFact) <= 24);
        CHECK(sizeof(LValueAssignmentFact) <= 68);
        // NodeIndex.h
        CHECK(sizeof(IndexedContainerSpan) <= 24);
        CHECK(sizeof(IndexedSyntaxNode) <= 24);
        CHECK(sizeof(IndexedSelectionRange) <= 32);
        CHECK(sizeof(NodeIndex) <= 248);
        // ScopeTree.h
        CHECK(sizeof(LocalDefinition) <= 192);
        CHECK(sizeof(LocalReference) <= 80);
        CHECK(sizeof(Scope) <= 128);
        // Symbol.h
        CHECK(sizeof(TypeQualifiers) == 1);
        CHECK(sizeof(SymbolModifiers) <= 4);
        CHECK(sizeof(ParameterInformation) <= 200);
        CHECK(sizeof(FunctionSignature) <= 240);
        CHECK(sizeof(VariableSignature) <= 192);
        CHECK(sizeof(ClassSignature) <= 80);
        CHECK(sizeof(FuncdefSignature) <= 152);
        CHECK(sizeof(Symbol) <= 504);
    }
}
