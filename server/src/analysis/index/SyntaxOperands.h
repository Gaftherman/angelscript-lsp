#pragma once

#include "analysis/index/NodeIndex.h"

namespace angel_lsp::analysis
{
/**
 * @brief Reads the direct child links of an indexed syntax node.
 * @note The node and index must refer to the same live tree. Interactive callers may omit the index.
 */
class SyntaxOperands
{
  public:
    SyntaxOperands(TSNode node, const NodeIndex* index)
        : m_node(node), m_index(index), m_fact(index ? index->FindOperandSyntax(node) : nullptr)
    {
    }

    [[nodiscard]] TSNode Field(TSFieldId field) const noexcept
    {
        if (!m_fact)
        {
            return parser::GetChildByField(m_node, field);
        }
        if (field != 0)
        {
            for (const auto* child = m_index->SyntaxNode(m_fact->first); child;
                 child = m_index->SyntaxNode(child->next))
            {
                if (child->field == field)
                {
                    return m_index->SyntaxHandle(*child);
                }
            }
        }
        return {};
    }

    template <typename Predicate> void ForEachNamed(Predicate&& predicate) const
    {
        if (!m_fact)
        {
            parser::ForEachNamedChildNodeUntil(m_node, predicate);
            return;
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->named && !predicate(m_index->SyntaxHandle(*child)))
            {
                return;
            }
        }
    }

    [[nodiscard]] TSNode FirstNamed(TSSymbol symbol = 0) const noexcept
    {
        if (!m_fact && symbol == 0)
        {
            return ts_node_is_null(m_node) ? TSNode{} : ts_node_named_child(m_node, 0);
        }

        TSNode match{};
        ForEachNamed(
            [&](TSNode child)
            {
                if (symbol == 0 || ts_node_symbol(child) == symbol)
                {
                    match = child;
                    return false;
                }
                return true;
            });
        return match;
    }

    /// Returns a named child by ordinal, including comments and recovery nodes.
    [[nodiscard]] TSNode NamedAt(uint32_t ordinal) const noexcept
    {
        if (!m_fact)
        {
            return ts_node_is_null(m_node) ? TSNode{} : ts_node_named_child(m_node, ordinal);
        }
        TSNode result{};
        ForEachNamed(
            [&](TSNode child)
            {
                if (ordinal == 0)
                {
                    result = child;
                    return false;
                }
                --ordinal;
                return true;
            });
        return result;
    }

    /// Reads a named field, falling back to the recovery ordinal when the field is absent.
    [[nodiscard]] TSNode FieldOrNamed(TSFieldId field, uint32_t ordinal) const noexcept
    {
        const TSNode result = Field(field);
        return ts_node_is_null(result) ? NamedAt(ordinal) : result;
    }

    /// Returns the sole named child, or an empty handle when the arity differs.
    [[nodiscard]] TSNode OnlyNamed() const noexcept
    {
        TSNode result{};
        ForEachNamed(
            [&](TSNode child)
            {
                if (!ts_node_is_null(result))
                {
                    result = {};
                    return false;
                }
                result = child;
                return true;
            });
        return result;
    }

    /// Reads the function, type or recovery child used as a call's callee.
    [[nodiscard]] TSNode Callee() const noexcept
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSNode function = Field(gs.fieldFunction);
        if (!ts_node_is_null(function))
        {
            return function;
        }
        const TSNode type = Field(gs.fieldType);
        return ts_node_is_null(type) ? NamedAt(0) : type;
    }

    /// Tests the operator field of a unary expression without following the operand.
    [[nodiscard]] bool HasUnaryOperator(TSSymbol symbol) const noexcept
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (ts_node_is_null(m_node) || ts_node_symbol(m_node) != gs.symUnaryExpression)
        {
            return false;
        }
        const TSNode op = Field(gs.fieldOperator);
        return !ts_node_is_null(op) && ts_node_symbol(op) == symbol;
    }

    /// Reads the initializer field or the recovery child immediately after '='.
    [[nodiscard]] TSNode DeclaratorValue(std::string_view sourceCode) const noexcept
    {
        if (!m_fact)
        {
            return parser::FindDeclaratorValueNode(m_node, sourceCode);
        }
        const TSNode value = Field(parser::GetGrammarSymbols().fieldValue);
        if (!ts_node_is_null(value))
        {
            return value;
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->afterAssign)
            {
                return m_index->SyntaxHandle(*child);
            }
        }
        return {};
    }

  private:
    TSNode m_node;
    const NodeIndex* m_index;
    const IndexedSyntaxNode* m_fact;
};

/// Follows the first named operand of each parenthesized expression.
[[nodiscard]] inline TSNode UnwrapIndexedParentheses(TSNode node, const NodeIndex* nodeIndex) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression)
    {
        const TSNode inner = SyntaxOperands(node, nodeIndex).FirstNamed();
        if (ts_node_is_null(inner))
        {
            break;
        }
        node = inner;
    }
    return node;
}

} // namespace angel_lsp::analysis
