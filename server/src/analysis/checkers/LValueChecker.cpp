#include "analysis/checkers/LValueChecker.h"

#include "analysis/checkers/LValueCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/StopFlag.h"
#include "utils/Utils.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Validates that the left-hand target of an assignment expression is an assignable l-value.
 * @param[in] node Assignment expression AST node.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in] scope Lexical scope active at assignment location.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckAssignmentTarget(TSNode node, const AstCheckContext& request, const Scope* scope,
                                  DiagnosticContext& ctx)
{
    TSNode target = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(target))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targetSym = ts_node_symbol(target);

    if (targetSym == gs.symCallExpression)
    {
        CheckCallLValue(target, request, scope, ctx);
        return;
    }

    TSNode unwrapped = parser::UnwrapParentheses(target);
    if (ts_node_symbol(unwrapped) == gs.symCallExpression)
    {
        CheckCallLValue(unwrapped, request, scope, ctx);
        return;
    }

    if (targetSym == gs.symTernaryExpression)
    {
        return;
    }

    if (!LValueNodeValidator::IsAssignableLValueNode(target, scope, ctx))
    {
        if (const auto dt = IsBareDataType(target, ExpressionTypeContext(scope, ctx)))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(target), diagnostics::codes::ExpressionIsDataType, *dt);
        }
        else
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(target), "as-err-not-lvalue");
        }
    }
}

/**
 * @brief Traverses indexed assignment expressions and evaluates left-hand target l-values.
 * @param[in] request Contextual request bundle containing pre-indexed syntax tree and scope root.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckLValuesIndexed(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
    {
        return;
    }

    for (const TSNode& node : request.nodeIndex->Nodes(parser::GetGrammarSymbols().symAssignmentExpression))
    {
        if (ctx.request.stopFlag && ctx.request.stopFlag->stop_requested())
        {
            return;
        }
        const TSPoint start = ts_node_start_point(node);
        const Scope* scope = FindInnermostScope(request.scopeRoot, start.row, start.column);
        CheckAssignmentTarget(node, request, scope, ctx);
    }
}

/**
 * @brief Check l values.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
void CheckLValues(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.rootNode) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (request.nodeIndex)
    {
        CheckLValuesIndexed(request, ctx);
        return;
    }

    NodeIndex localIndex(request.rootNode);
    AstCheckContext indexedRequest = request;
    indexedRequest.nodeIndex = &localIndex;
    CheckLValuesIndexed(indexedRequest, ctx);
}

} // namespace angel_lsp::analysis
