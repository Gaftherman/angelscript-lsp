#include "analysis/index/BinaryExpressionSyntax.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Converts a node's start and end points into a line/column source range.
 * @param[in] node Node to measure; a null node yields an empty range.
 * @return Zero-based range covering the node's text.
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
    return ts_node_is_null(node) ? IndexedByteRange{}
                                 : IndexedByteRange{ts_node_start_byte(node), ts_node_end_byte(node)};
}

/**
 * @brief Applies one child token or field of a lambda parameter list to a compact parameter fact.
 * @param[in] child Child syntax node inside a lambda parameter list.
 * @param[in] fieldId Field ID connecting child to the parameter list.
 * @param[in] source Document source text.
 * @param[in,out] current Compact lambda parameter fact being populated.
 */
static void ApplyLambdaParamNode(TSNode child, TSFieldId fieldId, std::string_view source,
                                 CallLambdaParamFact& current) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(child);
    if (fieldId == gs.fieldParamType)
    {
        current.typeBytes = BytesOf(child);
        current.hasWrittenType = true;
        const std::string_view text = parser::NodeText(child, source);
        current.isConst = IsConstTypeView(text);
        current.isHandle = IsHandleTypeView(text);
    }
    else if (sym == gs.tokAmp)
    {
        current.isReference = true;
    }
    else if (fieldId == gs.fieldDirection || sym == gs.tokIn || sym == gs.tokOut || sym == gs.tokInout)
    {
        current.isReference = true;
        current.modifier = (sym == gs.tokIn)    ? ParameterModifier::In
                           : (sym == gs.tokOut) ? ParameterModifier::Out
                                                : ParameterModifier::InOut;
    }
}

/**
 * @brief Extracts compact lambda parameter metadata into CallSiteFacts::lambdaParams.
 * @param[in] lambdaNode Lambda expression syntax node.
 * @param[in] index Precomputed node index of the document.
 * @param[in,out] facts Call site facts container owning lambdaParams.
 * @param[in,out] fact Call argument fact receiving the pool slice.
 */
static void ReadLambdaArgumentParams(TSNode lambdaNode, const NodeIndex& index, CallSiteFacts& facts,
                                     CallArgumentFact& fact)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode paramList = SyntaxOperands(lambdaNode, &index).Field(gs.fieldParameters);
    if (ts_node_is_null(paramList))
    {
        return;
    }
    fact.hasLambdaParameterList = true;
    const uint32_t begin = static_cast<uint32_t>(facts.lambdaParams.size());
    CallLambdaParamFact current;
    bool inParam = false;
    SyntaxOperands(paramList, &index)
        .ForEachChild(
            [&](TSNode child, TSFieldId field)
            {
                const TSSymbol sym = ts_node_symbol(child);
                if (sym == gs.tokCloseParen)
                {
                    return false;
                }
                if (sym == gs.tokOpenParen || sym == gs.symComment)
                {
                    return true;
                }
                if (sym == gs.tokComma)
                {
                    if (inParam)
                    {
                        facts.lambdaParams.push_back(current);
                        current = CallLambdaParamFact{};
                        inParam = false;
                    }
                    return true;
                }
                inParam = true;
                ApplyLambdaParamNode(child, field, facts.source, current);
                return true;
            });
    if (inParam)
    {
        facts.lambdaParams.push_back(current);
    }
    fact.detailIndex = begin;
    fact.detailCount = static_cast<uint16_t>(facts.lambdaParams.size() - begin);
}

CallArgumentFact ReadCallArgument(TSNode exprNode, TSNode nameNode, const NodeIndex& index, CallSiteFacts& facts)
{
    const auto& gs = parser::GetGrammarSymbols();
    CallArgumentFact fact;
    fact.bytes = BytesOf(exprNode);
    fact.range = RangeOf(exprNode);
    if (!ts_node_is_null(nameNode))
    {
        fact.isNamed = true;
        fact.nameBytes = BytesOf(nameNode);
    }
    fact.isThis = (!ts_node_is_null(exprNode) && ts_node_symbol(exprNode) == gs.symThisExpression);
    fact.isNullLiteral = IsNullInitializer(exprNode, &index);
    fact.isNumericLiteral = IsNumericLiteralExpression(exprNode, &index);

    const TSNode unwrapped = UnwrapIndexedParentheses(exprNode, &index);
    const TSSymbol sym = ts_node_is_null(unwrapped) ? 0 : ts_node_symbol(unwrapped);
    if (sym == gs.symLambdaExpression)
    {
        fact.kind = CallArgumentKind::Lambda;
        ReadLambdaArgumentParams(unwrapped, index, facts, fact);
    }
    else if (sym == gs.symInitializerList)
    {
        fact.kind = CallArgumentKind::InitializerList;
    }
    else if (sym == gs.symTernaryExpression)
    {
        fact.kind = CallArgumentKind::Ternary;
        const SyntaxOperands ternarySyntax(unwrapped, &index);
        const TSNode consequence = ternarySyntax.Field(gs.fieldConsequence);
        const TSNode alternative = ternarySyntax.Field(gs.fieldAlternative);
        fact.detailIndex = static_cast<uint32_t>(facts.ternaryBranches.size());
        fact.detailCount = 1;
        facts.ternaryBranches.push_back(CallArgumentTernaryFact{BytesOf(consequence), BytesOf(alternative)});
    }
    else if (sym == gs.symMemberExpression)
    {
        fact.kind = CallArgumentKind::Member;
        const SyntaxOperands memSyntax(unwrapped, &index);
        const TSNode obj = memSyntax.Field(gs.fieldObject);
        const TSNode mem = memSyntax.Field(gs.fieldMember);
        if (!ts_node_is_null(obj) && !ts_node_is_null(mem) && parser::NodeText(obj, facts.source) == "this")
        {
            fact.isThisMemberAccess = true;
            fact.detailIndex = static_cast<uint32_t>(facts.thisMemberNames.size());
            fact.detailCount = 1;
            facts.thisMemberNames.push_back(BytesOf(mem));
        }
    }
    else if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        fact.kind = CallArgumentKind::Identifier;
        fact.foldedBytes = BytesOf(unwrapped);
    }
    else if (sym == gs.symNullLiteral || sym == gs.symNumberLiteral || sym == gs.symStringLiteral ||
             sym == gs.tokTrue || sym == gs.tokFalse)
    {
        fact.kind = CallArgumentKind::Literal;
    }
    return fact;
}

void ExtractCallArguments(TSNode argListNode, const NodeIndex& index, CallSiteFacts& facts)
{
    if (ts_node_is_null(argListNode))
    {
        return;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(argListNode);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return;
    }
    TSNode exprNode{};
    TSNode nameNode{};
    auto flush = [&]()
    {
        if (!ts_node_is_null(exprNode))
        {
            facts.arguments.push_back(ReadCallArgument(exprNode, nameNode, index, facts));
            exprNode = TSNode{};
            nameNode = TSNode{};
        }
    };
    do
    {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_symbol(child);
        if (sym == gs.tokOpenParen || sym == gs.symComment || sym == gs.tokColon)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            flush();
            continue;
        }
        if (ts_tree_cursor_current_field_id(&cursor) == gs.fieldArgName)
        {
            nameNode = child;
        }
        else
        {
            if (!ts_node_is_null(exprNode))
            {
                flush();
            }
            exprNode = child;
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));
    ts_tree_cursor_delete(&cursor);
    flush();
}

} // namespace angel_lsp::analysis
