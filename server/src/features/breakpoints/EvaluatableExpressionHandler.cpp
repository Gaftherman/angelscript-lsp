#include "features/breakpoints/EvaluatableExpressionHandler.h"

#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"

#include <string_view>

namespace angel_lsp::features
{

namespace
{

bool IsNodeInsideCallFunction(TSNode memberExpr, const parser::GrammarSymbols& gs)
{
    TSNode parent = ts_node_parent(memberExpr);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symCallExpression)
    {
        return false;
    }
    TSNode fnChild = ts_node_child_by_field_id(parent, gs.fieldFunction);
    return !ts_node_is_null(fnChild) && ts_node_eq(fnChild, memberExpr);
}

EvaluatableExpressionItem MakeItem(TSNode node, std::string_view sourceCode)
{
    TSPoint start = ts_node_start_point(node);
    TSPoint end = ts_node_end_point(node);
    uint32_t startByte = ts_node_start_byte(node);
    uint32_t endByte = ts_node_end_byte(node);

    EvaluatableExpressionItem item;
    item.line = start.row;
    item.column = start.column;
    item.endLine = end.row;
    item.endColumn = end.column;
    if (startByte < endByte && endByte <= sourceCode.size())
    {
        item.expression = std::string(sourceCode.substr(startByte, endByte - startByte));
    }
    return item;
}

std::optional<EvaluatableExpressionItem> HandleMemberExpression(TSNode leaf, TSNode parent, std::string_view sourceCode,
                                                                const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeItem(leaf, sourceCode);
    }

    TSNode memberNode = ts_node_child_by_field_id(parent, gs.fieldMember);
    if (!ts_node_is_null(memberNode) && ts_node_eq(memberNode, leaf))
    {
        if (IsNodeInsideCallFunction(parent, gs))
        {
            return std::nullopt;
        }
        return MakeItem(parent, sourceCode);
    }

    return MakeItem(leaf, sourceCode);
}

std::optional<EvaluatableExpressionItem> HandleIndexExpression(TSNode leaf, TSNode parent, std::string_view sourceCode,
                                                               const parser::GrammarSymbols& gs)
{
    TSNode objNode = ts_node_child_by_field_id(parent, gs.fieldObject);
    if (!ts_node_is_null(objNode) && ts_node_eq(objNode, leaf))
    {
        return MakeItem(leaf, sourceCode);
    }
    return MakeItem(parent, sourceCode);
}

bool IsIgnoredNodeType(std::string_view type)
{
    return type.empty() || type == parser::nodes::Comment || type == parser::nodes::StringLiteral || type == ";" ||
           type == "{" || type == "}";
}

} // namespace

std::optional<EvaluatableExpressionItem>
EvaluatableExpressionHandler::Extract(const EvaluatableExpressionRequest& request)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    TSPoint point{request.line, request.column};
    TSNode leaf = ts_node_descendant_for_point_range(request.rootNode, point, point);
    if (ts_node_is_null(leaf))
    {
        return std::nullopt;
    }

    std::string_view leafType = ts_node_type(leaf);
    if (IsIgnoredNodeType(leafType))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode parent = ts_node_parent(leaf);

    if (leafType == "[" || leafType == "]")
    {
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symIndexExpression)
        {
            return MakeItem(parent, request.sourceCode);
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
            return HandleMemberExpression(leaf, parent, request.sourceCode, gs);
        }
        else if (parentSym == gs.symIndexExpression)
        {
            return HandleIndexExpression(leaf, parent, request.sourceCode, gs);
        }
    }

    return MakeItem(leaf, request.sourceCode);
}

} // namespace angel_lsp::features
