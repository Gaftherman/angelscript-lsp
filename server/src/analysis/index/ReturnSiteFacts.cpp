#include "analysis/index/ReturnSiteFacts.h"

#include "analysis/index/SpanFactSearch.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

const ReturnSiteFact* ReturnSiteFacts::Find(IndexedByteRange bytes) const noexcept
{
    return FindExactSpan(returns, bytes);
}

/**
 * @brief Converts a node's start and end points to a diagnostic range.
 * @param[in] node Non-null syntax node.
 * @return Line and column span of the node.
 */
static SourceRange RangeOf(TSNode node) noexcept
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    return {start.row, start.column, end.row, end.column};
}

/**
 * @brief Fills the owner data of a return statement that sits in a function declaration.
 * @param[in] function Function declaration node.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text.
 * @param[in,out] fact Return fact that receives the owner fields; left at None without a written return type.
 */
static void ReadFunctionOwner(TSNode function, const NodeIndex& index, std::string_view source, ReturnSiteFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(function, &index);
    TSNode typeNode = syntax.Field(gs.fieldReturnType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = syntax.Field(gs.fieldType);
    }
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    const std::string_view typeText = parser::NodeText(typeNode, source);
    const auto* syntaxFact = index.FindOperandSyntax(function);
    fact.owner = ReturnOwnerKind::Function;
    fact.returnType = typeText;
    fact.ownerRange = RangeOf(function);
    fact.returnsReference = IsReferenceTypeView(typeText) || (syntaxFact && syntaxFact->hasReturnReferenceToken);
}

ReturnSiteFacts BuildReturnSiteFacts(const NodeIndex& index, std::string_view source)
{
    const auto& gs = parser::GetGrammarSymbols();
    ReturnSiteFacts facts;
    for (const TSNode node : index.Nodes(gs.symReturnStatement))
    {
        const TSNode value = SyntaxOperands(node, &index).FieldOrNamed(gs.fieldValue);
        if (ts_node_is_null(value))
        {
            continue;
        }

        ReturnSiteFact fact;
        fact.bytes = {ts_node_start_byte(node), ts_node_end_byte(node)};
        fact.statementRange = RangeOf(node);
        fact.value = BuildBinaryOperandFact(value, index, source);

        const TSNode owner = index.EnclosingDeclaration(node);
        if (!ts_node_is_null(owner) && ts_node_symbol(owner) == gs.symLambdaExpression)
        {
            fact.owner = ReturnOwnerKind::Lambda;
            fact.lambdaTarget = ReadLambdaTarget(owner, source);
        }
        else if (!ts_node_is_null(owner) && ts_node_symbol(owner) == gs.symFuncDeclaration)
        {
            ReadFunctionOwner(owner, index, source, fact);
        }
        facts.returns.push_back(fact);
    }
    SortBySpan(facts.returns);
    return facts;
}

} // namespace angel_lsp::analysis
