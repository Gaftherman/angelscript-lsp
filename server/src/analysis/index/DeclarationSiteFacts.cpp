#include "analysis/index/DeclarationSiteFacts.h"

#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/ValueExpressionFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the variable declaration that spans exactly the given bytes.
 * @param[in] bytes Byte span of the declaration node.
 * @return Matching fact, or nullptr when the node was not indexed.
 */
const VariableDeclarationSiteFact* DeclarationSiteFacts::FindVariableDeclaration(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(variableDeclarations, bytes);
}

/**
 * @brief Finds the call or construct call that spans exactly the given bytes.
 * @param[in] bytes Byte span of the call node.
 * @return Matching fact, or nullptr when the node was not indexed.
 */
const CallConstructSiteFact* DeclarationSiteFacts::FindCallConstruct(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(callConstructs, bytes);
}

/**
 * @brief Finds the constructor delegation site that spans exactly the given bytes.
 * @param[in] bytes Byte span of the function declaration node.
 * @return Matching fact, or nullptr when the function is not a delegating constructor.
 */
const ConstructorDelegationSiteFact*
DeclarationSiteFacts::FindConstructorDelegation(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(constructorDelegations, bytes);
}

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Syntax node.
 * @return Line and column span of the node, or a zero range if null.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Converts a node to its byte span.
 * @param[in] node Syntax node.
 * @return Start and end byte of the node, or a zero range if null.
 */
static IndexedByteRange BytesOf(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    return {ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Reads a single argument into a construction argument fact.
 * @param[in] singleArg Syntax node of the single argument; may be null.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @return Construction argument fact; present is true only when singleArg is non-null.
 */
static ConstructionArgumentFact ReadConstructionArgument(TSNode singleArg, const NodeIndex& index,
                                                         std::string_view source)
{
    ConstructionArgumentFact fact;
    if (ts_node_is_null(singleArg))
    {
        return fact;
    }
    fact.present = true;
    fact.operand = BuildBinaryOperandFact(singleArg, index, source);
    fact.value = ReadValueExpression(singleArg, &index, source);
    return fact;
}

/**
 * @brief Captures the spans, initializer value and construction argument of a variable declarator.
 * @param[in] declarator Variable declarator syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @return Variable declarator fact.
 */
static VariableDeclaratorFact ReadVariableDeclarator(TSNode declarator, const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(declarator, &index);
    VariableDeclaratorFact fact;
    fact.bytes = BytesOf(declarator);
    fact.range = RangeOf(declarator);

    const TSNode nameNode = syntax.Field(gs.fieldName);
    fact.nameRange = RangeOf(!ts_node_is_null(nameNode) ? nameNode : declarator);

    const TSNode valueNode = syntax.Field(gs.fieldValue);
    if (!ts_node_is_null(valueNode))
    {
        fact.hasValue = true;
        fact.valueIsInitializerList = ts_node_symbol(valueNode) == gs.symInitializerList;
        fact.valueOperand = BuildBinaryOperandFact(valueNode, index, source);
        fact.value = ReadValueExpression(valueNode, &index, source);
        fact.funcdefValue = ReadFuncdefValue(valueNode, index, source);
    }

    const TSNode argsNode = syntax.Field(gs.fieldArguments);
    fact.hasArguments = !ts_node_is_null(argsNode);
    if (fact.hasArguments)
    {
        fact.argument = ReadConstructionArgument(SyntaxOperands(argsNode, &index).OnlyNamed(), index, source);
    }
    return fact;
}

/**
 * @brief Collects all variable declarations and their declarators from the syntax index.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @param[in,out] facts Declaration site facts sink.
 */
static void CollectVariableDeclarations(const NodeIndex& index, std::string_view source, DeclarationSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symVariableDeclaration))
    {
        VariableDeclarationSiteFact declFact;
        declFact.bytes = BytesOf(node);
        declFact.range = RangeOf(node);

        const SyntaxOperands syntax(node, &index);
        TSNode typeNode = syntax.Field(gs.fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = syntax.Field(gs.fieldType);
        }
        if (!ts_node_is_null(typeNode))
        {
            declFact.typeText = parser::NodeText(typeNode, source);
        }

        declFact.declaratorBegin = static_cast<uint32_t>(facts.declarators.size());
        syntax.ForEachNamed(
            [&](TSNode child)
            {
                if (ts_node_symbol(child) == gs.symVariableDeclarator)
                {
                    facts.declarators.push_back(ReadVariableDeclarator(child, index, source));
                }
                return true;
            });
        declFact.declaratorCount = static_cast<uint32_t>(facts.declarators.size()) - declFact.declaratorBegin;
        facts.variableDeclarations.push_back(declFact);
    }
}

/**
 * @brief Captures the spans, callee text, argument and lambda funcdef of a call or construct call.
 * @param[in] node Call or construct call expression syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @return Call construct site fact.
 */
static CallConstructSiteFact ReadCallConstruct(TSNode node, const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, &index);
    CallConstructSiteFact fact;
    fact.bytes = BytesOf(node);
    fact.range = RangeOf(node);

    const TSNode calleeNode = syntax.Callee();
    if (!ts_node_is_null(calleeNode))
    {
        fact.calleeText = parser::NodeText(calleeNode, source);
    }

    const TSNode argsNode = syntax.Field(gs.fieldArguments);
    if (!ts_node_is_null(argsNode))
    {
        const TSNode soleArgument = SyntaxOperands(argsNode, &index).OnlyNamed();
        fact.argument = ReadConstructionArgument(soleArgument, index, source);
        if (!ts_node_is_null(soleArgument) && ts_node_symbol(soleArgument) == gs.symLambdaExpression)
        {
            fact.soleArgumentIsLambda = true;
            fact.soleArgumentFuncdef = ReadFuncdefValue(soleArgument, index, source);
        }
    }
    return fact;
}

/**
 * @brief Collects all call and construct call expressions from the syntax index.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @param[in,out] facts Declaration site facts sink.
 */
static void CollectCallConstructs(const NodeIndex& index, std::string_view source, DeclarationSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode node : index.Nodes(gs.symCallExpression))
    {
        facts.callConstructs.push_back(ReadCallConstruct(node, index, source));
    }
    for (const TSNode node : index.Nodes(gs.symConstructCallExpression))
    {
        facts.callConstructs.push_back(ReadCallConstruct(node, index, source));
    }
}

/**
 * @brief Collects all constructor delegation sites across function declarations in the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @param[in,out] facts Declaration site facts sink.
 */
static void CollectConstructorDelegations(const NodeIndex& index, std::string_view source, DeclarationSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode func : index.Nodes(gs.symFuncDeclaration))
    {
        const TSNode parent = index.EnclosingDeclaration(func);
        if (ts_node_is_null(parent))
        {
            continue;
        }

        const TSNode classNameNode = SyntaxOperands(parent, &index).Field(gs.fieldName);
        if (ts_node_is_null(classNameNode))
        {
            continue;
        }
        const std::string_view className = parser::NodeText(classNameNode, source);
        if (className.empty())
        {
            continue;
        }

        const SyntaxOperands function(func, &index);
        const TSNode funcNameNode = function.Field(gs.fieldName);
        if (ts_node_is_null(funcNameNode) || parser::NodeText(funcNameNode, source) != className)
        {
            continue;
        }

        const SyntaxOperands body = function.FieldOperands(gs.fieldBody);
        if (body.IsNull())
        {
            continue;
        }

        const uint32_t callBegin = static_cast<uint32_t>(facts.delegationCalls.size());
        body.ForEachNamed(
            [&](TSNode statement)
            {
                if (ts_node_symbol(statement) != gs.symExpressionStatement)
                {
                    return true;
                }
                const SyntaxOperands exprOperands = SyntaxOperands(statement, &index).FirstNamedOperands();
                if (exprOperands.IsNull())
                {
                    return true;
                }
                const TSSymbol exprSym = exprOperands.Symbol();
                if (exprSym == gs.symCallExpression || exprSym == gs.symConstructCallExpression)
                {
                    const TSNode callee = exprOperands.Callee();
                    if (!ts_node_is_null(callee) && parser::NodeText(callee, source) == className)
                    {
                        const TSNode exprNode = exprOperands.Node();
                        if (!ts_node_is_null(exprNode))
                        {
                            facts.delegationCalls.push_back(RangeOf(exprNode));
                        }
                    }
                }
                return true;
            });

        const uint32_t callCount = static_cast<uint32_t>(facts.delegationCalls.size()) - callBegin;
        if (callCount > 0)
        {
            ConstructorDelegationSiteFact fact;
            fact.bytes = BytesOf(func);
            fact.callBegin = callBegin;
            fact.callCount = callCount;
            facts.constructorDelegations.push_back(fact);
        }
    }
}

/**
 * @brief Captures variable declarations, calls, construct calls and constructor delegations of a document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; the facts' views point into it.
 * @return Declaration site facts, ordered by source position.
 */
DeclarationSiteFacts BuildDeclarationSiteFacts(const NodeIndex& index, std::string_view source)
{
    DeclarationSiteFacts facts;
    CollectVariableDeclarations(index, source, facts);
    CollectCallConstructs(index, source, facts);
    CollectConstructorDelegations(index, source, facts);

    SortBySpan(facts.variableDeclarations);
    SortBySpan(facts.callConstructs);
    SortBySpan(facts.constructorDelegations);
    return facts;
}

} // namespace angel_lsp::analysis
