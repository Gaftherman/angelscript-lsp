#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ReferenceCounting.h"
#include "analysis/resolution/ReferenceCountingExprType.h"

#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Flags indicating which operator families appear in the active target set.
 */
struct OperatorTargetInterest
{
    bool hasAnyOperator = false;
    bool hasImplConv = false;
    bool hasImplCast = false;
    bool hasCall = false;
    bool hasIndex = false;
    bool hasCast = false;
    bool hasForeach = false;
    bool hasUnary = false;
    bool hasAssign = false;
    bool hasBinary = false;
};

/**
 * @brief Shared evaluation state for resolving operator sites within a single document scope tree.
 */
struct OperatorEvalContext
{
    std::string_view fileUri;
    const Scope* rootScope = nullptr;
    const IndexedOperatorSites& sites;
    const BatchReferenceContext& batch;
    const OperatorTargetInterest& interest;
    std::vector<std::string> exprMemo;
    std::vector<uint8_t> exprState;
};

/**
 * @brief Counts operator overload, conversion, foreach, and explicit operator call uses in a document.
 * @param[in] fileUri Document URI owning the scope tree.
 * @param[in] root Document root scope.
 * @param[in] ctx Batched reference count context with targets.
 */
void CountOperatorSiteReferences(std::string_view fileUri, const Scope* root, const BatchReferenceContext& ctx);

/**
 * @brief Parameters for finding a conversion operator method (`opImplConv`, `opImplCast`, `opConv`, `opCast`).
 */
struct ConversionLookupQuery
{
    std::string_view srcType;
    std::string_view targetType;
    std::string_view methodName;
    bool isSrcConst = false;
};

/**
 * @brief Records a matched operator Symbol against matching targets in the active evaluation context.
 * @param[in] winner Resolved operator overload symbol.
 * @param[in] line 0-based line of the usage site.
 * @param[in] col 0-based character column of the usage site.
 * @param[in] evalCtx Active document evaluation context.
 */
void RecordMatchedOperatorSymbol(const Symbol& winner, uint32_t line, uint32_t col, const OperatorEvalContext& evalCtx);

/**
 * @brief Evaluates call, foreach, and implicit conversion operator sites.
 * @param[in] site Operator site to evaluate.
 * @param[in,out] evalCtx Active document evaluation context.
 */
void EvaluateCallForeachOrConversionSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx);

/**
 * @brief Finds a conversion operator (`opImplConv`, `opImplCast`, `opConv`, `opCast`) matching targetType.
 * @param[in] query Conversion lookup parameters.
 * @param[in] symbolTable Workspace symbol table.
 * @return Matching conversion Symbol if found.
 */
[[nodiscard]] std::optional<Symbol> FindConversionOperator(const ConversionLookupQuery& query,
                                                           const SymbolTable& symbolTable);

/**
 * @brief Evaluates explicit operator calls, `opCall`, and implicit argument conversions for a call site.
 * @param[in] site Call site fact.
 * @param[in,out] evalCtx Active document evaluation context.
 * @param[in,out] ctx Expression type evaluation context.
 */
void EvaluateOperatorCallSite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx);

/**
 * @brief Evaluates a binary arithmetic, bitwise, shift, equality, or relational operator site.
 * @param[in] site Binary expression site fact.
 * @param[in,out] evalCtx Active document evaluation context.
 * @param[in,out] ctx Expression type evaluation context.
 */
void EvaluateBinarySite(const IndexedOperatorSite& site, OperatorEvalContext& evalCtx, ExprTypeEvalContext& ctx);
} // namespace angel_lsp::analysis
