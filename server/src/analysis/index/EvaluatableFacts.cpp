#include "analysis/index/EvaluatableFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <optional>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

static bool IsNodeInsideCallFunction(TSNode memberExpr, const parser::GrammarSymbols& gs)
{
    TSNode parent = parser::GetParent(memberExpr);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symCallExpression)
    {
        return false;
    }
    TSNode fnChild = ts_node_child_by_field_id(parent, gs.fieldFunction);
    return !ts_node_is_null(fnChild) && ts_node_eq(fnChild, memberExpr);
}

static IndexedEvaluatableSpan MakeEvaluatableSpan(TSNode node)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    IndexedEvaluatableSpan span;
    span.range = SourceRange{start.row, start.column, end.row, end.column};
    span.startByte = ts_node_start_byte(node);
    span.endByte = ts_node_end_byte(node);
    return span;
}

static std::optional<IndexedEvaluatableSpan> HandleMemberExpression(TSNode leaf, TSNode parent,
                                                                    const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeEvaluatableSpan(leaf);
    }

    TSNode memberNode = ts_node_child_by_field_id(parent, gs.fieldMember);
    if (!ts_node_is_null(memberNode) && ts_node_eq(memberNode, leaf))
    {
        if (IsNodeInsideCallFunction(parent, gs))
        {
            return std::nullopt;
        }
        return MakeEvaluatableSpan(parent);
    }

    return MakeEvaluatableSpan(leaf);
}

static std::optional<IndexedEvaluatableSpan> HandleIndexExpression(TSNode leaf, TSNode parent,
                                                                   const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeEvaluatableSpan(leaf);
    }
    return MakeEvaluatableSpan(parent);
}

static bool IsIgnoredNode(TSNode node, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(node))
    {
        return true;
    }
    const TSSymbol sym = ts_node_symbol(node);
    return sym == gs.symComment || sym == gs.symStringLiteral || sym == gs.tokOpenBrace || sym == gs.tokCloseBrace;
}

std::optional<IndexedEvaluatableSpan> FindEvaluatableSpan(const NodeIndex& index, uint32_t line, uint32_t column)
{
    TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    TSPoint point{line, column};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, point, point);
    if (ts_node_is_null(leaf))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    if (IsIgnoredNode(leaf, gs))
    {
        return std::nullopt;
    }

    TSNode parent = parser::GetParent(leaf);
    const TSSymbol leafSym = ts_node_symbol(leaf);

    if (leafSym == gs.tokOpenBracket || leafSym == gs.tokCloseBracket)
    {
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symIndexExpression)
        {
            return MakeEvaluatableSpan(parent);
        }
        return std::nullopt;
    }

    if (!ts_node_is_null(parent))
    {
        TSSymbol parentSym = ts_node_symbol(parent);
        if (parentSym == gs.symCallExpression)
        {
            TSNode fnNode = ts_node_child_by_field_id(parent, gs.fieldFunction);
            if (!ts_node_is_null(fnNode) && ts_node_eq(fnNode, leaf))
            {
                return std::nullopt;
            }
        }
        else if (parentSym == gs.symMemberExpression)
        {
            return HandleMemberExpression(leaf, parent, gs);
        }
        else if (parentSym == gs.symIndexExpression)
        {
            return HandleIndexExpression(leaf, parent, gs);
        }
    }

    return MakeEvaluatableSpan(leaf);
}

} // namespace angel_lsp::analysis
