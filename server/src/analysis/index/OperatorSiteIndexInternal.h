#pragma once

#include "analysis/index/OperatorSiteIndex.h"
#include "parser/GrammarSymbols.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{
/**
 * @brief Slot destination for attaching a child expression ID during iterative DAG construction.
 */
enum class ExprTargetSlot : uint8_t
{
    Root = 0,
    Left,
    Right,
    Extra,
    Arg
};

/**
 * @brief Pending expression node on the iterative First Pass builder worklist.
 */
struct PendingExprItem
{
    TSNode node{};
    uint32_t parentId = UINT32_MAX;
    uint32_t argPos = 0;
    ExprTargetSlot slot = ExprTargetSlot::Root;
};

/**
 * @brief Mutable builder state shared across operator site and expression extraction passes.
 */
struct OperatorSiteBuilder
{
    const NodeIndex& index;
    std::string_view source;
    const parser::GrammarSymbols& gs;
    IndexedOperatorSites& out;
    ankerl::unordered_dense::map<uint64_t, uint32_t> exprMemo;
};

/**
 * @brief Iteratively interns an expression AST subtree into the flat expression DAG.
 * @param[in] root Expression AST node.
 * @param[in,out] builder Active operator site builder state.
 * @return Index into builder.out.exprs, or UINT32_MAX if root is null or unrepresentable.
 */
[[nodiscard]] uint32_t InternOperatorExpr(TSNode root, OperatorSiteBuilder& builder);

/**
 * @brief Appends a boolean condition implicit-conversion site for an expression AST node.
 * @param[in] condNode Condition expression AST node.
 * @param[in,out] builder Active operator site builder state.
 */
void AppendBoolConditionSite(TSNode condNode, OperatorSiteBuilder& builder);

/**
 * @brief Extracts implicit conversion sites (variable initializers, returns, conditions) and using directives.
 * @param[in,out] builder Active operator site builder state.
 */
void CollectImplicitConversionAndUsingSites(OperatorSiteBuilder& builder);

/**
 * @brief Extracts call and foreach sites from the document index.
 * @param[in,out] builder Active operator site builder state.
 */
void CollectCallAndForeachSites(OperatorSiteBuilder& builder);
} // namespace angel_lsp::analysis
