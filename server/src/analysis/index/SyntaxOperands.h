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
    SyntaxOperands() noexcept = default;

    SyntaxOperands(TSNode node, const NodeIndex* index) noexcept
        : m_node(node), m_index(index), m_fact(index ? index->FindOperandSyntax(node) : nullptr)
    {
    }

    SyntaxOperands(const IndexedSyntaxNode* fact, const NodeIndex* index) noexcept
        : m_node{}, m_index(index), m_fact(fact)
    {
    }

    SyntaxOperands(const IndexedSyntaxNode* fact, TSNode node, const NodeIndex* index) noexcept
        : m_node(node), m_index(index), m_fact(fact)
    {
    }

    [[nodiscard]] TSNode Node() const noexcept
    {
        if (!ts_node_is_null(m_node))
        {
            return m_node;
        }
        if (m_fact && m_index)
        {
            m_node = m_index->SyntaxHandle(*m_fact);
        }
        return m_node;
    }

    [[nodiscard]] const IndexedSyntaxNode* Fact() const noexcept
    {
        return m_fact;
    }

    [[nodiscard]] const NodeIndex* Index() const noexcept
    {
        return m_index;
    }

    [[nodiscard]] TSSymbol Symbol() const noexcept
    {
        if (m_fact)
        {
            return m_fact->symbol;
        }
        return ts_node_is_null(m_node) ? 0 : ts_node_symbol(m_node);
    }

    [[nodiscard]] bool IsNull() const noexcept
    {
        return !m_fact && ts_node_is_null(m_node);
    }

    [[nodiscard]] SyntaxOperands FieldOperands(TSFieldId field) const noexcept
    {
        if (!m_fact || !m_index)
        {
            return SyntaxOperands(parser::GetChildByField(m_node, field), m_index);
        }
        if (field != 0)
        {
            for (const auto* child = m_index->SyntaxNode(m_fact->first); child;
                 child = m_index->SyntaxNode(child->next))
            {
                if (child->field == field)
                {
                    return SyntaxOperands(child, m_index);
                }
            }
        }
        return SyntaxOperands(TSNode{}, m_index);
    }

    [[nodiscard]] TSNode Field(TSFieldId field) const noexcept
    {
        return FieldOperands(field).Node();
    }

    [[nodiscard]] SyntaxOperands FirstNamedOperands(TSSymbol symbol = 0) const noexcept
    {
        if (!m_fact || !m_index)
        {
            if (symbol == 0)
            {
                return SyntaxOperands(ts_node_is_null(m_node) ? TSNode{} : ts_node_named_child(m_node, 0), m_index);
            }
            TSNode match{};
            parser::ForEachNamedChildNodeUntil(m_node,
                                               [&](TSNode child)
                                               {
                                                   if (ts_node_symbol(child) == symbol)
                                                   {
                                                       match = child;
                                                       return false;
                                                   }
                                                   return true;
                                               });
            return SyntaxOperands(match, m_index);
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->named && (symbol == 0 || child->symbol == symbol))
            {
                return SyntaxOperands(child, m_index);
            }
        }
        return SyntaxOperands(TSNode{}, m_index);
    }

    [[nodiscard]] TSNode FirstNamed(TSSymbol symbol = 0) const noexcept
    {
        return FirstNamedOperands(symbol).Node();
    }

    [[nodiscard]] SyntaxOperands NamedAtOperands(uint32_t ordinal) const noexcept
    {
        if (!m_fact || !m_index)
        {
            return SyntaxOperands(ts_node_is_null(m_node) ? TSNode{} : ts_node_named_child(m_node, ordinal), m_index);
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->named)
            {
                if (ordinal == 0)
                {
                    return SyntaxOperands(child, m_index);
                }
                --ordinal;
            }
        }
        return SyntaxOperands(TSNode{}, m_index);
    }

    [[nodiscard]] TSNode NamedAt(uint32_t ordinal) const noexcept
    {
        return NamedAtOperands(ordinal).Node();
    }

    [[nodiscard]] SyntaxOperands FieldOrNamedOperands(TSFieldId field, uint32_t ordinal = 0) const noexcept
    {
        const SyntaxOperands result = FieldOperands(field);
        return result.IsNull() ? NamedAtOperands(ordinal) : result;
    }

    [[nodiscard]] TSNode FieldOrNamed(TSFieldId field, uint32_t ordinal = 0) const noexcept
    {
        return FieldOrNamedOperands(field, ordinal).Node();
    }

    [[nodiscard]] SyntaxOperands OnlyNamedOperands() const noexcept
    {
        if (!m_fact || !m_index)
        {
            TSNode result{};
            parser::ForEachNamedChildNodeUntil(m_node,
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
            return SyntaxOperands(result, m_index);
        }
        const IndexedSyntaxNode* match = nullptr;
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->named)
            {
                if (match)
                {
                    return SyntaxOperands(TSNode{}, m_index);
                }
                match = child;
            }
        }
        return match ? SyntaxOperands(match, m_index) : SyntaxOperands(TSNode{}, m_index);
    }

    [[nodiscard]] TSNode OnlyNamed() const noexcept
    {
        return OnlyNamedOperands().Node();
    }

    [[nodiscard]] SyntaxOperands CalleeOperands() const noexcept
    {
        const auto& gs = parser::GetGrammarSymbols();
        const SyntaxOperands function = FieldOperands(gs.fieldFunction);
        if (!function.IsNull())
        {
            return function;
        }
        const SyntaxOperands type = FieldOperands(gs.fieldType);
        return type.IsNull() ? NamedAtOperands(0) : type;
    }

    [[nodiscard]] TSNode Callee() const noexcept
    {
        return CalleeOperands().Node();
    }

    [[nodiscard]] bool HasUnaryOperator(TSSymbol symbol) const noexcept
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (Symbol() != gs.symUnaryExpression)
        {
            return false;
        }
        const SyntaxOperands op = FieldOperands(gs.fieldOperator);
        return !op.IsNull() && op.Symbol() == symbol;
    }

    [[nodiscard]] SyntaxOperands DeclaratorValueOperands(std::string_view sourceCode) const noexcept
    {
        if (!m_fact || !m_index)
        {
            return SyntaxOperands(parser::FindDeclaratorValueNode(m_node, sourceCode), m_index);
        }
        const SyntaxOperands value = FieldOperands(parser::GetGrammarSymbols().fieldValue);
        if (!value.IsNull())
        {
            return value;
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->afterAssign)
            {
                return SyntaxOperands(child, m_index);
            }
        }
        return SyntaxOperands(TSNode{}, m_index);
    }

    [[nodiscard]] TSNode DeclaratorValue(std::string_view sourceCode) const noexcept
    {
        return DeclaratorValueOperands(sourceCode).Node();
    }

    template <typename Predicate> void ForEachChild(Predicate&& predicate) const
    {
        if (!m_fact || !m_index)
        {
            parser::ForEachChildNodeUntil(m_node, predicate);
            return;
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            const TSNode node = m_index->SyntaxHandle(*child);
            const bool keepGoing = [&]
            {
                if constexpr (requires { predicate(node, child->field); })
                {
                    return predicate(node, child->field);
                }
                else
                {
                    return predicate(node);
                }
            }();
            if (!keepGoing)
            {
                return;
            }
        }
    }

    template <typename Predicate> void ForEachNamed(Predicate&& predicate) const
    {
        if (!m_fact || !m_index)
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

    template <typename Predicate> void ForEachNamedOperands(Predicate&& predicate) const
    {
        if (!m_fact || !m_index)
        {
            parser::ForEachNamedChildNodeUntil(m_node,
                                               [&](TSNode child) { return predicate(SyntaxOperands(child, m_index)); });
            return;
        }
        for (const auto* child = m_index->SyntaxNode(m_fact->first); child; child = m_index->SyntaxNode(child->next))
        {
            if (child->named && !predicate(SyntaxOperands(child, m_index)))
            {
                return;
            }
        }
    }

  private:
    mutable TSNode m_node{};
    const NodeIndex* m_index = nullptr;
    const IndexedSyntaxNode* m_fact = nullptr;
};

[[nodiscard]] inline SyntaxOperands UnwrapIndexedParentheses(SyntaxOperands syntax) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!syntax.IsNull() && syntax.Symbol() == gs.symParenthesizedExpression)
    {
        const SyntaxOperands inner = syntax.FirstNamedOperands();
        if (inner.IsNull() || (syntax.Fact() && inner.Fact() == syntax.Fact()))
        {
            break;
        }
        syntax = inner;
    }
    return syntax;
}

[[nodiscard]] inline TSNode UnwrapIndexedParentheses(TSNode node, const NodeIndex* nodeIndex) noexcept
{
    return UnwrapIndexedParentheses(SyntaxOperands(node, nodeIndex)).Node();
}

} // namespace angel_lsp::analysis
