#pragma once

#include "analysis/index/NullFlowFacts.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Fills a NullFlowFacts arena from AST nodes using explicit worklists, never recursion.
 */
class NullFlowFactsBuilder
{
  public:
    /**
     * @brief Creates a builder that appends to an existing arena.
     * @param[in] index Node index used to read captured operand links.
     * @param[in] source Document text that node text views point into.
     * @param[in,out] facts Arena receiving nodes, declarators, parameters and callables.
     */
    NullFlowFactsBuilder(const NodeIndex& index, std::string_view source, NullFlowFacts& facts)
        : m_index(index), m_source(source), m_facts(facts)
    {
    }

    /**
     * @brief Adds an expression subtree and returns the id of its root.
     * @param[in] expr Expression node; a null node yields NoNullFlowNode.
     * @return Arena id of the root, or NoNullFlowNode.
     */
    NullFlowNodeId AddExpression(TSNode expr);

    /**
     * @brief Adds a statement subtree (blocks, declarations, if, return, expression statements).
     * @param[in] stmt Statement node; a null node yields NoNullFlowNode.
     * @return Arena id of the root, or NoNullFlowNode.
     */
    NullFlowNodeId AddStatement(TSNode stmt);

    /**
     * @brief Adds one function or lambda with its body and handle parameters; ignored when it has no body.
     * @param[in] callable Function declaration or lambda expression node.
     */
    void AddCallable(TSNode callable);

  private:
    /** @brief Creates the arena node for an AST node (kind, operator, range, text, flags) with no links. */
    NullFlowNodeId Declare(TSNode node);
    /** @brief Links the children of an expression node, declaring and queueing them. */
    void ExpandExpression(TSNode node, NullFlowNodeId id);
    /** @brief Links the children of a statement node, declaring and queueing them. */
    void ExpandStatement(TSNode node, NullFlowNodeId id);
    /** @brief Collects handle parameters of a callable into the arena and returns their count. */
    uint32_t AddParameters(TSNode callable);

    struct Pending
    {
        TSNode node;
        NullFlowNodeId id;
    };

    const NodeIndex& m_index;
    std::string_view m_source;
    NullFlowFacts& m_facts;
    std::vector<Pending> m_expressions;
    std::vector<Pending> m_statements;
};

} // namespace angel_lsp::analysis
