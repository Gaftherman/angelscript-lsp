#include "analysis/index/NullFlowFacts.h"

#include "analysis/index/NullFlowFactsInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/index/CallArgumentFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

static TSNode FindFunctionBody(TSNode funcNode, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(funcNode, &index);
    const TSNode body = syntax.Field(gs.fieldBody);
    return (!ts_node_is_null(body) && ts_node_symbol(body) == gs.symStatementBlock)
               ? body
               : syntax.FirstNamed(gs.symStatementBlock);
}

static TSNode FindParameterListNode(TSNode funcNode, const NodeIndex& index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands callable(funcNode, &index);
    TSNode params = callable.Field(gs.fieldParameters);
    if (ts_node_is_null(params))
    {
        callable.ForEachNamed(
            [&](TSNode child)
            {
                const auto symbol = ts_node_symbol(child);
                if (symbol == gs.symParameterList || symbol == gs.symLambdaParameterList)
                {
                    params = child;
                    return false;
                }
                return true;
            });
    }
    return params;
}

static TSNode ReadParameterType(const SyntaxOperands& parameter)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode type = parameter.Field(gs.fieldParamType);
    if (!ts_node_is_null(type))
    {
        return type;
    }
    const TSNode fallback = parameter.Field(gs.fieldType);
    if (!ts_node_is_null(fallback))
    {
        return fallback;
    }
    const TSNode recovered = parameter.FirstNamed();
    if (ts_node_is_null(recovered))
    {
        return {};
    }
    const auto sym = ts_node_symbol(recovered);
    return (sym == gs.symType || sym == gs.symPrimitiveType || sym == gs.symScopedIdentifier || sym == gs.symIdentifier)
               ? recovered
               : TSNode{};
}

static std::string ReadParameterName(const SyntaxOperands& parameter, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode nameField = parameter.Field(gs.fieldName);
    const std::string_view fieldText = parser::NodeText(nameField, source);
    if (!fieldText.empty())
    {
        return std::string(fieldText);
    }
    std::string fallback;
    parameter.ForEachNamed(
        [&](TSNode child)
        {
            const auto sym = ts_node_symbol(child);
            const std::string_view text = parser::NodeText(child, source);
            if (sym == gs.symIdentifier)
            {
                fallback = std::string(text);
            }
            else if (sym == gs.symScopedIdentifier && !utils::ContainsScopeSeparator(text))
            {
                fallback = std::string(text);
            }
            return true;
        });
    return fallback;
}

/**
 * @brief Traverses all functions and lambdas in the index to extract null-flow arena facts.
 * @param[in] index Precomputed document node index.
 * @param[in] source Source document text buffer.
 * @return Fully populated null-flow arena facts.
 */
NullFlowFacts BuildNullFlowFacts(const NodeIndex& index, std::string_view source)
{
    NullFlowFacts facts;
    NullFlowFactsBuilder builder(index, source, facts);
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode func : index.Nodes(gs.symFuncDeclaration))
    {
        builder.AddCallable(func);
    }
    for (const TSNode lambda : index.Nodes(gs.symLambdaExpression))
    {
        builder.AddCallable(lambda);
    }
    return facts;
}

/**
 * @brief Registers a callable entity (function or lambda) into the facts arena.
 * @param[in] callable Function declaration or lambda expression AST node.
 */
void NullFlowFactsBuilder::AddCallable(TSNode callable)
{
    const TSNode body = FindFunctionBody(callable, m_index);
    if (ts_node_is_null(body))
    {
        return;
    }
    const NullFlowNodeId bodyId = AddStatement(body);
    const TSPoint pt = ts_node_start_point(body);
    const uint32_t paramBegin = static_cast<uint32_t>(m_facts.parameters.size());
    const uint32_t paramCount = AddParameters(callable);
    m_facts.callables.push_back(NullFlowCallable{bodyId, pt.row, pt.column, paramBegin, paramCount});
}

/**
 * @brief Collects handle parameters declared by a callable into the facts arena.
 * @param[in] callable AST node of the callable.
 * @return Number of handle parameters appended to facts.parameters.
 */
uint32_t NullFlowFactsBuilder::AddParameters(TSNode callable)
{
    const TSNode params = FindParameterListNode(callable, m_index);
    if (ts_node_is_null(params))
    {
        return 0;
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(params) == gs.symLambdaParameterList)
    {
        uint32_t count = 0;
        for (const auto& lp : ExtractLambdaParameters(params, m_source, &m_index))
        {
            if (lp.isHandle && !lp.name.empty())
            {
                m_facts.parameters.push_back(NullFlowParameter{lp.name});
                ++count;
            }
        }
        return count;
    }
    uint32_t count = 0;
    SyntaxOperands(params, &m_index)
        .ForEachNamedOperands(
            [&](const SyntaxOperands& op)
            {
                const auto type = ExtractTypeInfoFromAST(ReadParameterType(op), m_source, &m_index);
                if (type.isHandle)
                {
                    auto name = ReadParameterName(op, m_source);
                    if (!name.empty())
                    {
                        m_facts.parameters.push_back(NullFlowParameter{std::move(name)});
                        ++count;
                    }
                }
                return true;
            });
    return count;
}

/**
 * @brief Adds a statement subtree and expands pending statements via a flat worklist.
 * @param[in] stmt AST node of the statement.
 * @return Node ID in the arena, or NoNullFlowNode if stmt is null.
 */
NullFlowNodeId NullFlowFactsBuilder::AddStatement(TSNode stmt)
{
    if (ts_node_is_null(stmt))
    {
        return NoNullFlowNode;
    }
    const NullFlowNodeId id = Declare(stmt);
    m_statements.push_back(Pending{stmt, id});
    for (size_t i = 0; i < m_statements.size(); ++i)
    {
        ExpandStatement(m_statements[i].node, m_statements[i].id);
    }
    m_statements.clear();
    return id;
}

/**
 * @brief Links statement children by symbol and queues nested statements onto the worklist.
 * @param[in] node AST node of the statement.
 * @param[in] id Arena ID of the declared statement.
 */
void NullFlowFactsBuilder::ExpandStatement(TSNode node, NullFlowNodeId id)
{
    if (ts_node_is_null(node))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const auto sym = ts_node_symbol(node);
    if (sym == gs.symStatementBlock)
    {
        NullFlowNodeId firstChild = NoNullFlowNode;
        NullFlowNodeId prevChild = NoNullFlowNode;
        SyntaxOperands(node, &m_index)
            .ForEachNamed(
                [&](TSNode child)
                {
                    const NullFlowNodeId childId = Declare(child);
                    firstChild = firstChild == NoNullFlowNode ? childId : firstChild;
                    if (prevChild != NoNullFlowNode)
                    {
                        m_facts.nodes[prevChild].next = childId;
                    }
                    prevChild = childId;
                    m_statements.push_back(Pending{child, childId});
                    return true;
                });
        m_facts.nodes[id].first = firstChild;
        return;
    }
    if (sym == gs.symVariableDeclaration)
    {
        const SyntaxOperands varDecl(node, &m_index);
        TSNode typeNode = varDecl.Field(gs.fieldVarType);
        typeNode = ts_node_is_null(typeNode) ? varDecl.Field(gs.fieldType) : typeNode;
        const TypeExtractionResult info = ExtractTypeInfoFromAST(typeNode, m_source, &m_index);
        const uint32_t typeNameIdx = static_cast<uint32_t>(m_facts.typeBaseNames.size());
        m_facts.typeBaseNames.push_back(info.baseTypeName);

        const uint32_t declBegin = static_cast<uint32_t>(m_facts.declarators.size());
        uint32_t declCount = 0;
        varDecl.ForEachNamedOperands(
            [&](const SyntaxOperands& op)
            {
                if (op.Symbol() == gs.symVariableDeclarator)
                {
                    const std::string_view name = parser::NodeText(op.Field(gs.fieldName), m_source);
                    TSNode val = op.Field(gs.fieldValue);
                    val = ts_node_is_null(val) ? op.Field(gs.fieldInit) : val;
                    m_facts.declarators.push_back(NullFlowDeclarator{name, AddExpression(val)});
                    ++declCount;
                }
                return true;
            });
        m_facts.nodes[id].typeIsHandle = info.isHandle;
        m_facts.nodes[id].typeNameIndex = typeNameIdx;
        m_facts.nodes[id].declaratorBegin = declBegin;
        m_facts.nodes[id].declaratorCount = declCount;
        return;
    }
    if (sym == gs.symExpressionStatement)
    {
        m_facts.nodes[id].first = AddExpression(SyntaxOperands(node, &m_index).FirstNamed());
        return;
    }
    if (sym == gs.symIfStatement)
    {
        const SyntaxOperands ifStmt(node, &m_index);
        const NullFlowNodeId condId = AddExpression(ifStmt.FieldOrNamed(gs.fieldCondition, 0));
        const TSNode consNode = ifStmt.FieldOrNamed(gs.fieldConsequence, 1);
        const NullFlowNodeId consId = ts_node_is_null(consNode) ? NoNullFlowNode : Declare(consNode);
        if (consId != NoNullFlowNode)
        {
            m_statements.push_back(Pending{consNode, consId});
        }
        const TSNode altNode = ifStmt.Field(gs.fieldAlternative);
        const NullFlowNodeId altId = ts_node_is_null(altNode) ? NoNullFlowNode : Declare(altNode);
        if (altId != NoNullFlowNode)
        {
            m_statements.push_back(Pending{altNode, altId});
        }
        m_facts.nodes[id].first = condId;
        m_facts.nodes[id].second = consId;
        m_facts.nodes[id].third = altId;
        return;
    }
    if (sym == gs.symReturnStatement)
    {
        m_facts.nodes[id].first = AddExpression(SyntaxOperands(node, &m_index).Field(gs.fieldValue));
        return;
    }
}

} // namespace angel_lsp::analysis
