#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/CallableResolutionInternal.h"
#include "analysis/resolution/AstRecoveryInternal.h"
#include "analysis/resolution/InlayTypeDeduction.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <string>

namespace angel_lsp::analysis
{

/**
 * @brief Helper to deduce the return type of a lambda body from return statements or funcdef target.
 * @param[in] node Lambda expression AST node.
 * @param[in] ctx Semantic expression type context.
 * @return Deduced return type string or empty string.
 */
static std::string DeduceLambdaReturnType(TSNode node, const ExpressionTypeContext& ctx)
{
    if (auto target = FuncdefTargetOfLambda(node, ctx.symbolTable, ctx.sourceCode))
    {
        return target->GetFuncdef().returnType;
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode body = parser::GetChildByField(node, gs.fieldBody);
    if (ts_node_is_null(body))
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i)
        {
            TSNode ch = ts_node_named_child(node, i);
            if (ts_node_symbol(ch) == gs.symStatementBlock)
            {
                body = ch;
                break;
            }
        }
    }
    if (!ts_node_is_null(body))
    {
        for (uint32_t i = 0; i < ts_node_named_child_count(body); ++i)
        {
            TSNode stmt = ts_node_named_child(body, i);
            if (ts_node_symbol(stmt) == gs.symReturnStatement)
            {
                TSNode retVal = parser::GetChildByField(stmt, gs.fieldValue);
                if (ts_node_is_null(retVal) && ts_node_named_child_count(stmt) > 0)
                {
                    retVal = ts_node_named_child(stmt, 0);
                }
                if (!ts_node_is_null(retVal))
                {
                    TSPoint retPt = ts_node_start_point(retVal);
                    const Scope* rootScope = ctx.scope;
                    while (rootScope && rootScope->parent)
                    {
                        rootScope = rootScope->parent;
                    }
                    const Scope* scope = rootScope ? FindInnermostScope(rootScope, retPt.row, retPt.column) : nullptr;
                    ExpressionTypeContext exprCtx(scope, ctx.symbolTable, ctx.sourceCode, ctx.uri);
                    exprCtx.nodeIndex = ctx.nodeIndex;
                    exprCtx.stringTypeName = ctx.stringTypeName;
                    exprCtx.arrayTypeName = ctx.arrayTypeName;
                    return ResolveExpressionType(retVal, exprCtx);
                }
                return "void";
            }
        }
    }
    return {};
}

std::string DeduceLambdaReturnTypeAt(const IndexedLambdaSite& site, const ExpressionTypeContext& ctx)
{
    TSNode node = RecoverNodeAt(ctx, site.startByte, site.endByte);
    if (ts_node_is_null(node))
    {
        return {};
    }
    TSPoint endPt = {site.returnTypeHintLine, site.returnTypeHintCharacter};
    const Scope* rootScope = ctx.scope;
    while (rootScope && rootScope->parent)
    {
        rootScope = rootScope->parent;
    }
    const Scope* scope = rootScope ? FindInnermostScope(rootScope, endPt.row, endPt.column) : nullptr;
    ExpressionTypeContext exprCtx(scope, ctx.symbolTable, ctx.sourceCode, ctx.uri);
    exprCtx.nodeIndex = ctx.nodeIndex;
    exprCtx.stringTypeName = ctx.stringTypeName;
    exprCtx.arrayTypeName = ctx.arrayTypeName;
    std::string deduced = ResolveExpressionType(node, exprCtx);
    if (deduced.empty())
    {
        deduced = DeduceLambdaReturnType(node, ctx);
    }
    return deduced;
}

} // namespace angel_lsp::analysis
