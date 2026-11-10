#include "analysis/checkers/ConstCheckerCommon.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks assignment expressions for mutations of const variables or read-only references.
 * @param[in] node AST node of the assignment expression.
 * @param[in] request Contextual request bundle containing AST root and source text.
 * @param[in] scope Lexical scope active at assignment location.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckAssignment(TSNode node, const AstCheckContext& request, const Scope* scope, DiagnosticContext& ctx)
{
    TSNode target = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(target))
    {
        return;
    }

    bool isHandleAssignment = false;
    TSNode actualTarget = target;
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(target) == gs.symUnaryExpression)
    {
        TSNode opNode = parser::GetChildByField(target, gs.fieldOperator);
        if (!ts_node_is_null(opNode) && ts_node_symbol(opNode) == gs.tokAt)
        {
            isHandleAssignment = true;
            TSNode operand = parser::GetChildByField(target, gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                actualTarget = operand;
            }
        }
    }

    const ExpressionTypeContext constCtx(scope, ctx);
    const auto encInfo = FindEnclosingMethod(node, request.sourceCode);

    std::string memberName;
    if (IsMemberOfThis(actualTarget, encInfo, constCtx, memberName))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(target), "as-err-readonly-reference", memberName);
        return;
    }

    if (isHandleAssignment)
    {
        if (ResolveHandleConstness(actualTarget, constCtx) == Constness::Const)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(target), "as-err-const-assignment",
                                std::string(parser::NodeText(target, request.sourceCode)));
        }
        return;
    }

    if (ResolveConstness(target, constCtx) != Constness::Const)
    {
        return;
    }

    ctx.EmitAtNodeRange(ToOptionalSourceRange(target), "as-err-const-assignment",
                        std::string(parser::NodeText(target, request.sourceCode)));
}

} // namespace angel_lsp::analysis
