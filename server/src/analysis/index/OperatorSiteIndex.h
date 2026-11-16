#pragma once

#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Syntactic category of an AST-free indexed expression node.
 */
enum class IndexedExprKind : uint8_t
{
    Unknown = 0,
    Identifier,
    This,
    Literal,
    Null,
    Member,
    Call,
    Index,
    Binary,
    Assign,
    Unary,
    Postfix,
    Cast,
    Construct,
    Ternary,
    InitList
};

/**
 * @brief Flat AST-free expression DAG node for operator and conversion resolution.
 */
struct IndexedExprNode
{
    IndexedExprKind kind = IndexedExprKind::Unknown;
    uint32_t left = UINT32_MAX;
    uint32_t right = UINT32_MAX;
    uint32_t extra = UINT32_MAX;
    uint32_t argBegin = 0;
    uint32_t argCount = 0;
    uint32_t startByte = 0;
    uint32_t line = 0;
    uint32_t character = 0;
    std::string text;
    bool isHandle : 1 = false;
    bool isScoped : 1 = false;
};

/**
 * @brief Kind of operator or implicit conversion site captured during First Pass.
 */
enum class OperatorSiteKind : uint8_t
{
    Binary,
    Assignment,
    Unary,
    Index,
    Cast,
    Call,
    Foreach,
    ImplicitConversion
};

/**
 * @brief Pre-extracted operator, call, foreach, or conversion site in a document.
 */
struct IndexedOperatorSite
{
    OperatorSiteKind kind = OperatorSiteKind::Binary;
    uint32_t line = 0;
    uint32_t character = 0;
    uint32_t scopeLine = 0;
    uint32_t scopeCharacter = 0;
    uint32_t exprId = UINT32_MAX;
    uint32_t rhsExprId = UINT32_MAX;
    uint32_t argBegin = 0;
    uint32_t argCount = 0;
    std::string op;
    std::string targetType;
    bool leftIsHandle : 1 = false;
    bool rightIsHandle : 1 = false;
    bool isPostfix : 1 = false;
    bool isTargetHandle : 1 = false;
    bool isCondition : 1 = false;
    bool isMemberCall : 1 = false;
};

/**
 * @brief Using-namespace directive with its lexical scope byte span.
 */
struct IndexedUsingDirective
{
    std::string namespaceName;
    uint32_t startByte = 0;
    uint32_t scopeEndByte = UINT32_MAX;
};

/**
 * @brief Document-level AST-free expression graph and operator site table.
 */
struct IndexedOperatorSites
{
    std::vector<IndexedExprNode> exprs;
    std::vector<uint32_t> argIds;
    std::vector<IndexedOperatorSite> sites;
    std::vector<IndexedUsingDirective> usings;
};

/**
 * @brief Extracts operator, conversion, call, foreach, and using-directive sites from a document index.
 * @param[in] index Precomputed NodeIndex of the document.
 * @param[in] sourceCode Document source text.
 * @return Shared immutable IndexedOperatorSites structure.
 */
[[nodiscard]] std::shared_ptr<const IndexedOperatorSites> BuildOperatorSites(const NodeIndex& index,
                                                                             std::string_view sourceCode);
} // namespace angel_lsp::analysis
