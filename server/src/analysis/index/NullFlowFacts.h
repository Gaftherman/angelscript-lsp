#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Document-local key of a node in a NullFlowFacts arena.
 */
using NullFlowNodeId = uint32_t;

/**
 * @brief Marks an absent link between NullFlowFacts nodes.
 */
inline constexpr NullFlowNodeId NoNullFlowNode = UINT32_MAX;

/**
 * @brief Syntactic role of a node that the null-safety flow analysis distinguishes.
 */
enum class NullFlowNodeKind : uint8_t
{
    Other,
    Block,
    VariableDeclaration,
    ExpressionStatement,
    IfStatement,
    ReturnStatement,
    Identifier,
    ScopedIdentifier,
    Parenthesized,
    Unary,
    Assignment,
    Binary,
    Member,
    Index,
    Call,
    Cast,
    Ternary,
    ConstructCall,
    This
};

/**
 * @brief Operator token of a Unary or Binary node, reduced to the cases null tracking reacts to.
 */
enum class NullFlowOperator : uint8_t
{
    None,
    Is,
    EqEq,
    NotIs,
    NotEq,
    And,
    Or,
    Not,
    Other
};

/**
 * @brief One statement or expression, with children addressed by arena id instead of AST handle.
 * @note Link roles per kind are listed on the first, second, third and next fields.
 */
struct NullFlowNode
{
    SourceRange range;
    std::string_view text;                 ///< Source spelling of the whole node; views the analysed document text.
    std::string_view typeText;             ///< Cast only: spelling of the target type node; empty otherwise.
    NullFlowNodeId first = NoNullFlowNode; ///< Block: first statement; If/Ternary: condition; Return/Cast: value;
                                           ///< Unary: operand; Assignment/Binary: left; Member/Index: object; Call:
                                           ///< callee; Parenthesized/ExpressionStatement: first named child.
    NullFlowNodeId second =
        NoNullFlowNode; ///< If/Ternary: consequence; Assignment/Binary: right; Index: index; Call: first argument.
    NullFlowNodeId third = NoNullFlowNode; ///< If/Ternary: alternative.
    NullFlowNodeId next = NoNullFlowNode;  ///< Next sibling statement of a Block or next argument of a Call.
    uint32_t declaratorBegin = 0; ///< VariableDeclaration only: first slice index into NullFlowFacts::declarators.
    uint32_t declaratorCount = 0; ///< VariableDeclaration only: number of declarators.
    uint32_t typeNameIndex = 0;   ///< VariableDeclaration only: index into NullFlowFacts::typeBaseNames.
    NullFlowNodeKind kind = NullFlowNodeKind::Other;
    NullFlowOperator op = NullFlowOperator::None;
    bool singleCharOperator = false; ///< Unary only: operator token is exactly one byte long.
    bool complete = false;           ///< Binary only: operator, left and right operands were all captured.
    bool soleOperand = false;        ///< Parenthesized only: `first` is its only named child.
    bool isNullInitializer = false;  ///< Node is the null literal.
    bool typeIsHandle = false;       ///< VariableDeclaration and Cast: the declared or target type is a handle.
};

/**
 * @brief A single declarator of a variable declaration statement.
 */
struct NullFlowDeclarator
{
    std::string_view name; ///< Views the analysed document text.
    NullFlowNodeId value = NoNullFlowNode;
};

/**
 * @brief A handle parameter that starts a callable as possibly null.
 */
struct NullFlowParameter
{
    std::string name; ///< Owned: lambda parameter names come from the resolution layer as owned text.
};

/**
 * @brief A function or lambda body together with the handle parameters declared by its signature.
 */
struct NullFlowCallable
{
    NullFlowNodeId body = NoNullFlowNode;
    uint32_t bodyLine = 0;   ///< Zero-based start row of the body, used to look up the enclosing scope.
    uint32_t bodyColumn = 0; ///< Zero-based start column of the body.
    uint32_t parameterBegin = 0;
    uint32_t parameterCount = 0;
};

/**
 * @brief AST-free arena of everything the null-safety flow analysis reads from the syntax tree.
 * @note Text views point into the source passed to BuildNullFlowFacts, which must outlive this object.
 */
struct NullFlowFacts
{
    std::vector<NullFlowNode> nodes;
    std::vector<NullFlowDeclarator> declarators;
    std::vector<NullFlowParameter> parameters;
    std::vector<std::string> typeBaseNames; ///< Base type name per VariableDeclaration, by typeNameIndex.
    std::vector<NullFlowCallable> callables;

    /**
     * @brief Looks up a node by id.
     * @param[in] id Arena key, possibly NoNullFlowNode.
     * @return The node, or nullptr when id is absent or out of range.
     */
    [[nodiscard]] const NullFlowNode* Find(NullFlowNodeId id) const noexcept
    {
        return id < nodes.size() ? &nodes[id] : nullptr;
    }
};

/**
 * @brief Extracts the null-safety flow arena for every function and lambda that has a body.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Facts with one callable per body, functions first, then lambdas, each in document order.
 */
[[nodiscard]] NullFlowFacts BuildNullFlowFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
