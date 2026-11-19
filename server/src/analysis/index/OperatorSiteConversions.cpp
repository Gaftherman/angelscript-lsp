#include "analysis/index/OperatorSiteIndexInternal.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/Primitives.h"

#include <utility>

namespace angel_lsp::analysis
{
void AppendBoolConditionSite(TSNode condNode, OperatorSiteBuilder& builder)
{
    const uint32_t exprId = InternOperatorExpr(condNode, builder);
    if (exprId == UINT32_MAX)
    {
        return;
    }
    const TSPoint pt = ts_node_start_point(condNode);
    IndexedOperatorSite site;
    site.kind = OperatorSiteKind::ImplicitConversion;
    site.line = pt.row;
    site.character = pt.column;
    site.scopeLine = pt.row;
    site.scopeCharacter = pt.column;
    site.exprId = exprId;
    site.targetType = "bool";
    site.isCondition = true;
    builder.out.sites.push_back(std::move(site));
}

static void CollectVariableInitConversionSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    for (const TSNode declNode : builder.index.Nodes(gs.symVariableDeclaration))
    {
        const TSNode typeNode = parser::GetChildByField(declNode, gs.fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            continue;
        }
        const std::string_view typeText = parser::NodeText(typeNode, builder.source);
        if (typeText.empty() || parser::primitives::IsAutoType(typeText))
        {
            continue;
        }
        const bool isTargetHandle = IsHandleTypeView(typeText);
        TSTreeCursor cursor = ts_tree_cursor_new(declNode);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                const TSNode child = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(child) != gs.symVariableDeclarator)
                {
                    continue;
                }
                const TSNode valNode = parser::GetChildByField(child, gs.fieldValue);
                if (ts_node_is_null(valNode))
                {
                    continue;
                }
                const TSSymbol valSym = ts_node_symbol(valNode);
                if (valSym == gs.symInitializerList || valSym == gs.symTypedInitializerList)
                {
                    continue;
                }
                const uint32_t exprId = InternOperatorExpr(valNode, builder);
                if (exprId == UINT32_MAX)
                {
                    continue;
                }
                const TSPoint pt = ts_node_start_point(valNode);
                IndexedOperatorSite site;
                site.kind = OperatorSiteKind::ImplicitConversion;
                site.line = pt.row;
                site.character = pt.column;
                site.scopeLine = pt.row;
                site.scopeCharacter = pt.column;
                site.exprId = exprId;
                site.targetType = std::string(typeText);
                site.isTargetHandle = isTargetHandle;
                builder.out.sites.push_back(std::move(site));
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
    }
}

void CollectImplicitConversionAndUsingSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    CollectVariableInitConversionSites(builder);

    for (const TSNode retNode : builder.index.Nodes(gs.symReturnStatement))
    {
        const TSNode valNode = parser::GetChildByField(retNode, gs.fieldValue);
        if (ts_node_is_null(valNode))
        {
            continue;
        }
        const uint32_t exprId = InternOperatorExpr(valNode, builder);
        if (exprId == UINT32_MAX)
        {
            continue;
        }
        const TSPoint pt = ts_node_start_point(valNode);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::ImplicitConversion;
        site.line = pt.row;
        site.character = pt.column;
        site.scopeLine = pt.row;
        site.scopeCharacter = pt.column;
        site.exprId = exprId;
        builder.out.sites.push_back(std::move(site));
    }

    auto visitCondStmt = [&](TSNode stmt)
    {
        TSNode cond = parser::GetChildByField(stmt, gs.fieldCondition);
        if (!ts_node_is_null(cond) && ts_node_symbol(cond) == gs.symExpressionStatement)
        {
            cond = SyntaxOperands(cond, &builder.index).FirstNamed();
        }
        if (!ts_node_is_null(cond))
        {
            AppendBoolConditionSite(cond, builder);
        }
    };
    for (const TSNode s : builder.index.Nodes(gs.symIfStatement))
    {
        visitCondStmt(s);
    }
    for (const TSNode s : builder.index.Nodes(gs.symWhileStatement))
    {
        visitCondStmt(s);
    }
    for (const TSNode s : builder.index.Nodes(gs.symDoWhileStatement))
    {
        visitCondStmt(s);
    }
    for (const TSNode s : builder.index.Nodes(gs.symForStatement))
    {
        visitCondStmt(s);
    }
    for (const TSNode s : builder.index.Nodes(gs.symTernaryExpression))
    {
        visitCondStmt(s);
    }

    for (const TSNode usingNode : builder.index.Nodes(gs.symUsingDeclaration))
    {
        const TSNode nameNode = parser::GetChildByField(usingNode, gs.fieldName);
        if (ts_node_is_null(nameNode))
        {
            continue;
        }
        const TSNode parent = parser::GetParent(usingNode);
        const uint32_t scopeEnd = ts_node_is_null(parent) ? UINT32_MAX : ts_node_end_byte(parent);
        builder.out.usings.push_back(IndexedUsingDirective{std::string(parser::NodeText(nameNode, builder.source)),
                                                           ts_node_start_byte(usingNode), scopeEnd});
    }
}

void CollectCallAndForeachSites(OperatorSiteBuilder& builder)
{
    const auto& gs = builder.gs;
    for (const TSNode node : builder.index.Nodes(gs.symCallExpression))
    {
        const uint32_t callId = InternOperatorExpr(node, builder);
        if (callId == UINT32_MAX)
        {
            continue;
        }
        const auto& expr = builder.out.exprs[callId];
        TSNode funcNode = UnwrapIndexedParentheses(parser::GetChildByField(node, gs.fieldFunction), &builder.index);
        TSPoint callPt = ts_node_start_point(node);
        std::string opName;
        bool isMember = false;
        if (!ts_node_is_null(funcNode))
        {
            const TSSymbol funcSym = ts_node_symbol(funcNode);
            if (funcSym == gs.symMemberExpression)
            {
                TSNode mem = parser::GetChildByField(funcNode, gs.fieldMember);
                if (!ts_node_is_null(mem))
                {
                    callPt = ts_node_start_point(mem);
                    opName = std::string(parser::NodeText(mem, builder.source));
                    isMember = true;
                }
            }
            else if (funcSym == gs.symIdentifier)
            {
                callPt = ts_node_start_point(funcNode);
                opName = std::string(parser::NodeText(funcNode, builder.source));
            }
            else if (funcSym == gs.symScopedIdentifier)
            {
                const uint32_t count = ts_node_named_child_count(funcNode);
                TSNode tail = count > 0 ? ts_node_named_child(funcNode, count - 1) : funcNode;
                callPt = ts_node_start_point(tail);
                opName = std::string(parser::NodeText(tail, builder.source));
            }
        }
        const TSPoint scopePt = ts_node_start_point(node);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Call;
        site.line = callPt.row;
        site.character = callPt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = expr.left;
        site.argBegin = expr.argBegin;
        site.argCount = expr.argCount;
        site.op = std::move(opName);
        site.isMemberCall = isMember;
        builder.out.sites.push_back(std::move(site));
    }

    for (const TSNode node : builder.index.Nodes(gs.symForeachStatement))
    {
        const TSNode colNode = parser::GetChildByField(node, gs.fieldCollection);
        if (ts_node_is_null(colNode))
        {
            continue;
        }
        uint32_t varCount = 0;
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                if (ts_node_symbol(ts_tree_cursor_current_node(&cursor)) == gs.symForeachVariable)
                {
                    ++varCount;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);

        const TSPoint pt = ts_node_start_point(node);
        const TSPoint scopePt = ts_node_start_point(colNode);
        IndexedOperatorSite site;
        site.kind = OperatorSiteKind::Foreach;
        site.line = pt.row;
        site.character = pt.column;
        site.scopeLine = scopePt.row;
        site.scopeCharacter = scopePt.column;
        site.exprId = InternOperatorExpr(colNode, builder);
        site.argCount = varCount;
        builder.out.sites.push_back(std::move(site));
    }
}
} // namespace angel_lsp::analysis
