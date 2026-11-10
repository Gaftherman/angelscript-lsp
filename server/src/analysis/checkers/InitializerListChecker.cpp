#include "analysis/checkers/InitializerListChecker.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <array>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
/**
 * @brief The declared return type of the function a `return` sits in, or "" when unknown.
 */
static std::string EnclosingReturnType(TSNode returnNode, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> targets = {gs.symLambdaExpression, gs.symFuncDeclaration};
    TSNode enclosing = parser::FindAncestorWithAnySymbol(parser::GetParent(returnNode), targets);
    if (!ts_node_is_null(enclosing) && ts_node_symbol(enclosing) == gs.symFuncDeclaration)
    {
        TSNode returnType = parser::GetChildByField(enclosing, gs.fieldReturnType);
        if (ts_node_is_null(returnType))
        {
            returnType = parser::GetChildByField(enclosing, gs.fieldType);
        }
        return ts_node_is_null(returnType) ? std::string() : std::string(parser::NodeText(returnType, sourceCode));
    }
    return "";
}

/**
 * @brief Computes the element context at the position of an initializer list node.
 * @param[in] node    Target AST node.
 * @param[in] request Analysis request.
 * @return ElementContext with enclosing scope.
 */
static ElementContext ElementsAt(TSNode node, const AstCheckContext& request)
{
    const TSPoint start = ts_node_start_point(node);
    return ElementContext{request.sourceCode,
                          request.scopeRoot ? FindEnclosingScope(request.scopeRoot, start.row, start.column) : nullptr};
}

/**
 * @brief Validates typed initializer lists (`Type = { ... }`).
 * @param[in] node    Typed initializer list AST node.
 * @param[in] initCtx Initializer traversal context.
 */
static void ProcessTypedInitializerList(TSNode node, const InitializerListContext& initCtx)
{
    TSNode typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldType);
    TSNode valueNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldValue);
    if (!ts_node_is_null(typeNode) && !ts_node_is_null(valueNode))
    {
        const ElementContext elements = ElementsAt(valueNode, initCtx.request);
        const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.structCache, 0};
        InitializerListValidator::ValidateList(
            valueNode, std::string(parser::NodeText(typeNode, initCtx.request.sourceCode)), valCtx);
    }
}

/**
 * @brief Validates assignment expressions with initializer list right-hand sides (`a = { ... }`).
 * @param[in] node    Assignment expression AST node.
 * @param[in] initCtx Initializer traversal context.
 */
static void ProcessAssignmentExpression(TSNode node, const InitializerListContext& initCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode value = parser::GetChildByField(node, gs.fieldRight);
    if (ts_node_is_null(value) || ts_node_symbol(value) != gs.symInitializerList)
    {
        return;
    }

    TSNode opNode = parser::GetChildByField(node, gs.fieldOperator);
    TSNode target = parser::GetChildByField(node, gs.fieldLeft);
    if (!ts_node_is_null(opNode) && !ts_node_is_null(target) && ts_node_symbol(opNode) == gs.tokAssign)
    {
        const ElementContext elements = ElementsAt(value, initCtx.request);
        const std::string targetType =
            ResolveExpressionType(target, ExpressionTypeContext(elements.scopeRoot, initCtx.ctx));
        if (!targetType.empty())
        {
            const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.structCache, 0};
            InitializerListValidator::ValidateList(value, targetType, valCtx);
        }
    }
}

/**
 * @brief Validates return statements returning initializer lists (`return { ... };`).
 * @param[in] node    Return statement AST node.
 * @param[in] initCtx Initializer traversal context.
 */
static void ProcessReturnStatement(TSNode node, const InitializerListContext& initCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode value{};
    parser::ForEachChildNode(node,
                             [&](TSNode child)
                             {
                                 if (ts_node_is_null(value) && ts_node_is_named(child) &&
                                     ts_node_symbol(child) != gs.symComment)
                                 {
                                     value = child;
                                 }
                             });
    if (ts_node_is_null(value) || ts_node_symbol(value) != gs.symInitializerList)
    {
        return;
    }

    const std::string returnType = EnclosingReturnType(node, initCtx.request.sourceCode);
    if (!returnType.empty())
    {
        const ElementContext elements = ElementsAt(value, initCtx.request);
        const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.structCache, 0};
        InitializerListValidator::ValidateList(value, returnType, valCtx);
    }
}

/**
 * @brief Validates variable declarations with initializer list initializers (`Type a = { ... };`).
 * @param[in] node    Variable declaration AST node.
 * @param[in] initCtx Initializer traversal context.
 */
static void ProcessVariableDeclaration(TSNode node, const InitializerListContext& initCtx)
{
    TSNode typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldType);
    }
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const std::string declaredType(parser::NodeText(typeNode, initCtx.request.sourceCode));
    parser::ForEachChildNode(
        node,
        [&](TSNode declarator)
        {
            if (!ts_node_is_named(declarator) || ts_node_symbol(declarator) != gs.symVariableDeclarator)
            {
                return;
            }

            TSNode valueNode = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldValue);
            if (!ts_node_is_null(valueNode) && ts_node_symbol(valueNode) == gs.symInitializerList)
            {
                const ElementContext elements = ElementsAt(valueNode, initCtx.request);
                const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.structCache, 0};
                InitializerListValidator::ValidateList(valueNode, declaredType, valCtx);
            }
        });
}

/**
 * @brief Dispatches an AST node to the corresponding initializer list processor.
 * @param[in] node    AST node to process.
 * @param[in] initCtx Initializer traversal context.
 */
static void DispatchInitListNode(TSNode node, const InitializerListContext& initCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symTypedInitializerList)
    {
        ProcessTypedInitializerList(node, initCtx);
    }
    else if (sym == gs.symAssignmentExpression)
    {
        ProcessAssignmentExpression(node, initCtx);
    }
    else if (sym == gs.symReturnStatement)
    {
        ProcessReturnStatement(node, initCtx);
    }
    else if (sym == gs.symVariableDeclaration)
    {
        ProcessVariableDeclaration(node, initCtx);
    }
}

/**
 * @brief Walks initializer list nodes using the fast NodeIndex cursor merge.
 * @param[in] initCtx Initializer traversal context.
 */
static void DispatchIndexedInitializerLists(const InitializerListContext& initCtx)
{
    const auto& gs = parser::GetGrammarSymbols();
    initCtx.request.nodeIndex->ForEachNodeOrdered(std::array{gs.symTypedInitializerList, gs.symAssignmentExpression,
                                                             gs.symReturnStatement, gs.symVariableDeclaration},
                                                  [&](TSNode node) { DispatchInitListNode(node, initCtx); });
}

/**
 * @brief Falls back to flat worklist traversal when NodeIndex is unavailable.
 * @param[in] initCtx Initializer traversal context.
 */
static void DispatchTreeWalkInitializerLists(const InitializerListContext& initCtx)
{
    if (ts_node_is_null(initCtx.request.rootNode))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(initCtx.request.rootNode);
    bool visitedChildren = false;
    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (!visitedChildren)
        {
            DispatchInitListNode(node, initCtx);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                continue;
            }
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            visitedChildren = false;
            continue;
        }
        if (ts_tree_cursor_goto_parent(&cursor))
        {
            visitedChildren = true;
            continue;
        }
        break;
    }
    ts_tree_cursor_delete(&cursor);
}

void CheckInitializerListAgainstType(TSNode listNode, const std::string& targetType, const ElementContext& elements,
                                     DiagnosticContext& ctx)
{
    StructLayoutCache cache;
    const ListValidationContext valCtx{ctx, elements, cache, 0};
    InitializerListValidator::ValidateList(listNode, targetType, valCtx);
}

void ValidateInitializerList(TSNode listNode, const std::string& expectedType, DiagnosticContext& ctx)
{
    ElementContext elements{ctx.request.sourceCode, ctx.request.scopeRoot.get()};
    StructLayoutCache cache;
    const ListValidationContext valCtx{ctx, elements, cache, 0};
    InitializerListValidator::ValidateList(listNode, expectedType, valCtx);
}

void CheckInitializerLists(const AstCheckContext& request, DiagnosticContext& ctx)
{
    StructLayoutCache structCache;
    const InitializerListContext initCtx{request, ctx, structCache};

    if (request.nodeIndex)
    {
        DispatchIndexedInitializerLists(initCtx);
    }
    else
    {
        DispatchTreeWalkInitializerLists(initCtx);
    }
}
} // namespace angel_lsp::analysis
