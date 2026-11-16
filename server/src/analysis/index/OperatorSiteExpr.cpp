#include "analysis/index/OperatorSiteIndexInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{
static std::string ClassifyNumberLiteralSpelling(std::string_view text)
{
    const bool isHex = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (isHex)
    {
        return "int";
    }
    bool isFloatSuffix = false;
    bool hasDecimalOrExp = false;
    for (const char ch : text)
    {
        if (ch == 'f' || ch == 'F')
        {
            isFloatSuffix = true;
        }
        else if (ch == '.' || ch == 'e' || ch == 'E' || ch == 'd' || ch == 'D')
        {
            hasDecimalOrExp = true;
        }
    }
    if (isFloatSuffix)
    {
        return "float";
    }
    return hasDecimalOrExp ? "double" : "int";
}

static std::string ExtractConstructTypeText(TSNode node, const parser::GrammarSymbols& gs, std::string_view source)
{
    TSNode argsNode = parser::GetChildByField(node, gs.fieldArguments);
    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_is_null(argsNode) ? ts_node_end_byte(node) : ts_node_start_byte(argsNode);
    if (start >= end || end > source.size())
    {
        return {};
    }
    std::string_view raw = source.substr(start, end - start);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n'))
    {
        raw.remove_suffix(1);
    }
    return std::string(raw);
}

static void CollectArgumentExprNodes(TSNode argsListNode, const parser::GrammarSymbols& gs,
                                     std::vector<TSNode>& outNodes)
{
    if (ts_node_is_null(argsListNode))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(argsListNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_named(child) || ts_node_symbol(child) == gs.symComment)
            {
                continue;
            }
            if (ts_tree_cursor_current_field_id(&cursor) == gs.fieldArgName)
            {
                continue;
            }
            outNodes.push_back(child);
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

static void CollectIndexArgNodes(TSNode indexNode, const parser::GrammarSymbols& gs, std::vector<TSNode>& outNodes)
{
    TSTreeCursor cursor = ts_tree_cursor_new(indexNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_tree_cursor_current_field_id(&cursor) == gs.fieldIndex)
            {
                outNodes.push_back(child);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

static void AttachExprSlot(const PendingExprItem& item, uint32_t id, IndexedOperatorSites& out)
{
    if (item.parentId == UINT32_MAX)
    {
        return;
    }
    auto& parent = out.exprs[item.parentId];
    switch (item.slot)
    {
    case ExprTargetSlot::Left:
        parent.left = id;
        break;
    case ExprTargetSlot::Right:
        parent.right = id;
        break;
    case ExprTargetSlot::Extra:
        parent.extra = id;
        break;
    case ExprTargetSlot::Arg:
        out.argIds[item.argPos] = id;
        break;
    case ExprTargetSlot::Root:
        break;
    }
}

static void PushArgWorklist(const std::vector<TSNode>& argNodes, uint32_t exprId, OperatorSiteBuilder& builder,
                            std::vector<PendingExprItem>& worklist)
{
    auto& expr = builder.out.exprs[exprId];
    expr.argBegin = static_cast<uint32_t>(builder.out.argIds.size());
    expr.argCount = static_cast<uint32_t>(argNodes.size());
    builder.out.argIds.resize(expr.argBegin + expr.argCount, UINT32_MAX);
    for (uint32_t i = 0; i < expr.argCount; ++i)
    {
        worklist.push_back({argNodes[i], exprId, expr.argBegin + i, ExprTargetSlot::Arg});
    }
}

static void PopulateExprChildren(TSNode node, uint32_t id, OperatorSiteBuilder& builder,
                                 std::vector<PendingExprItem>& worklist)
{
    const auto& gs = builder.gs;
    const TSSymbol sym = ts_node_symbol(node);
    auto& expr = builder.out.exprs[id];
    const TSPoint pt = ts_node_start_point(node);
    expr.startByte = ts_node_start_byte(node);
    expr.line = pt.row;
    expr.character = pt.column;

    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        expr.kind = IndexedExprKind::Identifier;
        expr.text = std::string(parser::NodeText(node, builder.source));
        // The grammar wraps even a bare name in scoped_identifier, so the node kind says nothing about `::`.
        expr.isScoped = utils::ContainsScopeSeparator(expr.text);
    }
    else if (sym == gs.symThisExpression)
    {
        expr.kind = IndexedExprKind::This;
    }
    else if (sym == gs.symNullLiteral)
    {
        expr.kind = IndexedExprKind::Null;
        expr.text = "null";
    }
    else if (sym == gs.symBooleanLiteral)
    {
        expr.kind = IndexedExprKind::Literal;
        expr.text = "bool";
    }
    else if (sym == gs.symStringLiteral || sym == gs.symConcatenatedString)
    {
        expr.kind = IndexedExprKind::Literal;
        expr.text = "string";
    }
    else if (sym == gs.symNumberLiteral)
    {
        expr.kind = IndexedExprKind::Literal;
        expr.text = ClassifyNumberLiteralSpelling(parser::NodeText(node, builder.source));
    }
    else if (sym == gs.symMemberExpression)
    {
        expr.kind = IndexedExprKind::Member;
        expr.text = std::string(parser::NodeText(parser::GetChildByField(node, gs.fieldMember), builder.source));
        worklist.push_back({parser::GetChildByField(node, gs.fieldObject), id, 0, ExprTargetSlot::Left});
    }
    else if (sym == gs.symCallExpression)
    {
        expr.kind = IndexedExprKind::Call;
        std::vector<TSNode> argNodes;
        CollectArgumentExprNodes(parser::GetChildByField(node, gs.fieldArguments), gs, argNodes);
        PushArgWorklist(argNodes, id, builder, worklist);
        worklist.push_back({parser::GetChildByField(node, gs.fieldFunction), id, 0, ExprTargetSlot::Left});
    }
    else if (sym == gs.symConstructCallExpression)
    {
        expr.kind = IndexedExprKind::Construct;
        expr.text = ExtractConstructTypeText(node, gs, builder.source);
        expr.isScoped = true;
        std::vector<TSNode> argNodes;
        CollectArgumentExprNodes(parser::GetChildByField(node, gs.fieldArguments), gs, argNodes);
        PushArgWorklist(argNodes, id, builder, worklist);
    }
    else if (sym == gs.symIndexExpression)
    {
        expr.kind = IndexedExprKind::Index;
        std::vector<TSNode> argNodes;
        CollectIndexArgNodes(node, gs, argNodes);
        PushArgWorklist(argNodes, id, builder, worklist);
        worklist.push_back({parser::GetChildByField(node, gs.fieldObject), id, 0, ExprTargetSlot::Left});
    }
    else if (sym == gs.symBinaryExpression || sym == gs.symAssignmentExpression)
    {
        const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
        const TSNode leftNode = parser::GetChildByField(node, gs.fieldLeft);
        const TSNode rightNode = parser::GetChildByField(node, gs.fieldRight);
        expr.kind = (sym == gs.symBinaryExpression) ? IndexedExprKind::Binary : IndexedExprKind::Assign;
        expr.text = std::string(parser::OperatorSpelling(opNode));
        expr.isHandle =
            (sym == gs.symAssignmentExpression) && SyntaxOperands(leftNode, &builder.index).HasUnaryOperator(gs.tokAt);
        worklist.push_back({leftNode, id, 0, ExprTargetSlot::Left});
        worklist.push_back({rightNode, id, 0, ExprTargetSlot::Right});
    }
    else if (sym == gs.symUnaryExpression || sym == gs.symPostfixExpression)
    {
        const TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
        expr.kind = (sym == gs.symUnaryExpression) ? IndexedExprKind::Unary : IndexedExprKind::Postfix;
        expr.text = std::string(parser::OperatorSpelling(opNode));
        expr.isHandle =
            (sym == gs.symUnaryExpression) && !ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt;
        worklist.push_back({parser::GetChildByField(node, gs.fieldOperand), id, 0, ExprTargetSlot::Left});
    }
    else if (sym == gs.symCastExpression || sym == gs.symFunctionalCastExpression)
    {
        expr.kind = IndexedExprKind::Cast;
        expr.text = std::string(parser::NodeText(parser::GetChildByField(node, gs.fieldType), builder.source));
        worklist.push_back({parser::GetChildByField(node, gs.fieldValue), id, 0, ExprTargetSlot::Left});
    }
    else if (sym == gs.symTernaryExpression)
    {
        expr.kind = IndexedExprKind::Ternary;
        worklist.push_back({parser::GetChildByField(node, gs.fieldCondition), id, 0, ExprTargetSlot::Left});
        worklist.push_back({parser::GetChildByField(node, gs.fieldConsequence), id, 0, ExprTargetSlot::Extra});
        worklist.push_back({parser::GetChildByField(node, gs.fieldAlternative), id, 0, ExprTargetSlot::Right});
    }
    else if (sym == gs.symInitializerList || sym == gs.symTypedInitializerList)
    {
        expr.kind = IndexedExprKind::InitList;
        if (sym == gs.symTypedInitializerList)
        {
            expr.text = std::string(parser::NodeText(parser::GetChildByField(node, gs.fieldType), builder.source));
        }
    }
}

uint32_t InternOperatorExpr(TSNode root, OperatorSiteBuilder& builder)
{
    uint32_t rootId = UINT32_MAX;
    std::vector<PendingExprItem> worklist;
    worklist.push_back({root, UINT32_MAX, 0, ExprTargetSlot::Root});

    while (!worklist.empty())
    {
        const PendingExprItem item = worklist.back();
        worklist.pop_back();

        TSNode node = UnwrapIndexedParentheses(item.node, &builder.index);
        if (ts_node_is_null(node))
        {
            continue;
        }

        const uint64_t key = (static_cast<uint64_t>(ts_node_start_byte(node)) << 32) | ts_node_end_byte(node);
        uint32_t id = UINT32_MAX;
        if (const auto found = builder.exprMemo.find(key); found != builder.exprMemo.end())
        {
            id = found->second;
        }
        else
        {
            id = static_cast<uint32_t>(builder.out.exprs.size());
            builder.out.exprs.emplace_back();
            builder.exprMemo.try_emplace(key, id);
            PopulateExprChildren(node, id, builder, worklist);
        }

        if (item.slot == ExprTargetSlot::Root)
        {
            rootId = id;
        }
        else
        {
            AttachExprSlot(item, id, builder.out);
        }
    }
    return rootId;
}
} // namespace angel_lsp::analysis
