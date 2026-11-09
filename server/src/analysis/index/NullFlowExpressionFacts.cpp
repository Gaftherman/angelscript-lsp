#include "analysis/index/NullFlowFactsInternal.h"
#include "analysis/index/Symbol.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Traverses child expression nodes according to syntactic role and queues them for expansion.
 * @param[in] node AST node corresponding to the expression.
 * @param[in] id Arena id of the node whose child links are populated.
 */
void NullFlowFactsBuilder::ExpandExpression(TSNode node, NullFlowNodeId id)
{
    if (id >= m_facts.nodes.size() || ts_node_is_null(node))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const NullFlowNodeKind kind = m_facts.nodes[id].kind;

    auto linkChild = [this](TSNode childNode) -> NullFlowNodeId
    {
        if (ts_node_is_null(childNode))
        {
            return NoNullFlowNode;
        }
        const NullFlowNodeId childId = Declare(childNode);
        m_expressions.push_back({childNode, childId});
        return childId;
    };

    switch (kind)
    {
    case NullFlowNodeKind::Parenthesized:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode firstNode = syntax.FirstNamed();
        const bool isSole = !ts_node_is_null(syntax.OnlyNamed());
        const NullFlowNodeId firstId = linkChild(firstNode);
        m_facts.nodes[id].first = firstId;
        m_facts.nodes[id].soleOperand = isSole;
        break;
    }
    case NullFlowNodeKind::Unary:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode operandNode = syntax.Field(gs.fieldOperand);
        const NullFlowNodeId operandId = linkChild(operandNode);
        m_facts.nodes[id].first = operandId;
        break;
    }
    case NullFlowNodeKind::Assignment:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode leftNode = syntax.Field(gs.fieldLeft);
        const NullFlowNodeId leftId = linkChild(leftNode);
        const TSNode rightNode = syntax.Field(gs.fieldRight);
        const NullFlowNodeId rightId = linkChild(rightNode);
        m_facts.nodes[id].first = leftId;
        m_facts.nodes[id].second = rightId;
        break;
    }
    case NullFlowNodeKind::Binary:
    {
        const SyntaxOperands syntax(node, &m_index);
        const NullFlowNodeId leftId = linkChild(syntax.Field(gs.fieldLeft));
        const NullFlowNodeId rightId = linkChild(syntax.Field(gs.fieldRight));
        m_facts.nodes[id].first = leftId;
        m_facts.nodes[id].second = rightId;
        break;
    }
    case NullFlowNodeKind::Member:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode objectNode = syntax.Field(gs.fieldObject);
        const NullFlowNodeId objectId = linkChild(objectNode);
        m_facts.nodes[id].first = objectId;
        break;
    }
    case NullFlowNodeKind::Index:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode objectNode = syntax.Field(gs.fieldObject);
        const NullFlowNodeId objectId = linkChild(objectNode);
        const TSNode indexNode = syntax.Field(gs.fieldIndex);
        const NullFlowNodeId indexId = linkChild(indexNode);
        m_facts.nodes[id].first = objectId;
        m_facts.nodes[id].second = indexId;
        break;
    }
    case NullFlowNodeKind::Call:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode functionNode = syntax.Field(gs.fieldFunction);
        const NullFlowNodeId functionId = linkChild(functionNode);

        NullFlowNodeId firstArgId = NoNullFlowNode;
        NullFlowNodeId prevArgId = NoNullFlowNode;

        const TSNode argsNode = syntax.Field(gs.fieldArguments);
        if (!ts_node_is_null(argsNode))
        {
            SyntaxOperands(argsNode, &m_index)
                .ForEachNamed(
                    [&](TSNode argNode)
                    {
                        const NullFlowNodeId argId = linkChild(argNode);
                        if (argId != NoNullFlowNode)
                        {
                            if (firstArgId == NoNullFlowNode)
                            {
                                firstArgId = argId;
                            }
                            if (prevArgId != NoNullFlowNode)
                            {
                                m_facts.nodes[prevArgId].next = argId;
                            }
                            prevArgId = argId;
                        }
                        return true;
                    });
        }

        m_facts.nodes[id].first = functionId;
        m_facts.nodes[id].second = firstArgId;
        break;
    }
    case NullFlowNodeKind::Cast:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode valueNode = syntax.Field(gs.fieldValue);
        const TSNode typeNode = syntax.Field(gs.fieldType);
        const std::string_view typeText = parser::NodeText(typeNode, m_source);
        const bool isHandle = ExtractTypeInfoFromAST(typeNode, m_source, &m_index).isHandle;
        const NullFlowNodeId valueId = linkChild(valueNode);
        m_facts.nodes[id].first = valueId;
        m_facts.nodes[id].typeText = typeText;
        m_facts.nodes[id].typeIsHandle = isHandle;
        break;
    }
    case NullFlowNodeKind::Ternary:
    {
        const SyntaxOperands syntax(node, &m_index);
        const TSNode conditionNode = syntax.Field(gs.fieldCondition);
        const NullFlowNodeId conditionId = linkChild(conditionNode);
        const TSNode consequenceNode = syntax.Field(gs.fieldConsequence);
        const NullFlowNodeId consequenceId = linkChild(consequenceNode);
        const TSNode alternativeNode = syntax.Field(gs.fieldAlternative);
        const NullFlowNodeId alternativeId = linkChild(alternativeNode);
        m_facts.nodes[id].first = conditionId;
        m_facts.nodes[id].second = consequenceId;
        m_facts.nodes[id].third = alternativeId;
        break;
    }
    default:
        break;
    }
}

/**
 * @brief Adds an expression subtree to the arena and iteratively expands all its descendant expressions.
 * @param[in] expr Root AST expression node; a null node yields NoNullFlowNode.
 * @return Arena id of the expression root, or NoNullFlowNode when expr is null.
 */
NullFlowNodeId NullFlowFactsBuilder::AddExpression(TSNode expr)
{
    if (ts_node_is_null(expr))
    {
        return NoNullFlowNode;
    }

    const NullFlowNodeId rootId = Declare(expr);
    m_expressions.push_back({expr, rootId});

    while (!m_expressions.empty())
    {
        const Pending pending = m_expressions.back();
        m_expressions.pop_back();
        ExpandExpression(pending.node, pending.id);
    }

    return rootId;
}

} // namespace angel_lsp::analysis
