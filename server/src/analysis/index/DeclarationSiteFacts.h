#pragma once

#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/ValueExpressionFacts.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief The single named argument of a construction, with the data its conversion check reads.
 */
struct ConstructionArgumentFact
{
    bool present = false;      ///< The argument list holds exactly one named child.
    BinaryOperandFact operand; ///< Span, range and spelling of the argument; anchors diagnostics.
    ValueExpressionFact value; ///< The argument read as a value expression; input of ResolveValueType.
};

/**
 * @brief One declarator of a variable declaration.
 */
struct VariableDeclaratorFact
{
    IndexedByteRange bytes;              ///< Byte span of the declarator.
    SourceRange range;                   ///< Line and column span of the declarator.
    SourceRange nameRange;               ///< Span of the name, or of the declarator when the name is missing.
    bool hasValue = false;               ///< An initializer value is present.
    bool valueIsInitializerList = false; ///< The initializer value is an initializer list.
    BinaryOperandFact valueOperand;      ///< Initializer value operand; anchors diagnostics.
    ValueExpressionFact value;           ///< Initializer value read as a value expression.
    FuncdefValueFact funcdefValue;       ///< Initializer value read as a funcdef value.
    bool hasArguments = false;           ///< A constructor argument list is present.
    ConstructionArgumentFact argument;   ///< Sole constructor argument; absent for other argument counts.
};

/**
 * @brief A variable declaration with its declared type spelling and a slice of declarators.
 */
struct VariableDeclarationSiteFact
{
    IndexedByteRange bytes;       ///< Byte span of the declaration.
    SourceRange range;            ///< Line and column span; its start selects the lexical scope.
    std::string_view typeText;    ///< Raw type spelling from the var-type field, else the type field.
    uint32_t declaratorBegin = 0; ///< First slice index into DeclarationSiteFacts::declarators.
    uint32_t declaratorCount = 0; ///< Number of declarators, in source order.
};

/**
 * @brief A call or explicit construct call expression.
 */
struct CallConstructSiteFact
{
    IndexedByteRange bytes;               ///< Byte span of the call expression.
    SourceRange range;                    ///< Line and column span; its start selects the lexical scope.
    std::string_view calleeText;          ///< Raw callee spelling.
    bool soleArgumentIsLambda = false;    ///< The only named argument is a lambda expression.
    FuncdefValueFact soleArgumentFuncdef; ///< The lambda read as a funcdef value; filled only for a lambda.
    ConstructionArgumentFact argument;    ///< Sole argument; absent for other argument counts.
};

/**
 * @brief A constructor whose body contains statements that call the class itself.
 */
struct ConstructorDelegationSiteFact
{
    IndexedByteRange bytes; ///< Byte span of the function declaration.
    uint32_t callBegin = 0; ///< First slice index into DeclarationSiteFacts::delegationCalls.
    uint32_t callCount = 0; ///< Number of offending statements, in source order.
};

/**
 * @brief AST-free variable declaration, call, construct and constructor delegation sites of one revision.
 * @note Views point into the source passed to BuildDeclarationSiteFacts, which must outlive this object.
 */
struct DeclarationSiteFacts
{
    std::vector<VariableDeclarationSiteFact> variableDeclarations;     ///< Ordered by start byte.
    std::vector<VariableDeclaratorFact> declarators;                   ///< Grouped by declaration, source order.
    std::vector<CallConstructSiteFact> callConstructs;                 ///< Ordered by start byte.
    std::vector<ConstructorDelegationSiteFact> constructorDelegations; ///< Ordered by start byte.
    std::vector<SourceRange> delegationCalls;                          ///< Grouped by constructor, source order.

    /**
     * @brief Finds the variable declaration that spans exactly the given bytes.
     * @param[in] bytes Byte span of the declaration node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const VariableDeclarationSiteFact* FindVariableDeclaration(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the call or construct call that spans exactly the given bytes.
     * @param[in] bytes Byte span of the call node.
     * @return Matching fact, or nullptr when the node was not indexed.
     */
    [[nodiscard]] const CallConstructSiteFact* FindCallConstruct(IndexedByteRange bytes) const noexcept;

    /**
     * @brief Finds the constructor delegation site that spans exactly the given bytes.
     * @param[in] bytes Byte span of the function declaration node.
     * @return Matching fact, or nullptr when the function is not a delegating constructor.
     */
    [[nodiscard]] const ConstructorDelegationSiteFact* FindConstructorDelegation(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Captures variable declarations, calls, construct calls and constructor delegations of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the facts' views point into it.
 * @return Declaration site facts, ordered by source position.
 */
[[nodiscard]] DeclarationSiteFacts BuildDeclarationSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
