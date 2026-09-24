/**
 * @file CodeActionExtractVariable.cpp
 * @brief Refactoring provider for Extract Variable code action.
 */

#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

constexpr std::string_view kExtractableExpressionTypes[] = {"binary_expression",
                                                            "unary_expression",
                                                            "postfix_expression",
                                                            "call_expression",
                                                            "member_expression",
                                                            "ternary_expression",
                                                            "cast_expression",
                                                            "functional_cast_expression",
                                                            "construct_call_expression",
                                                            "index_expression",
                                                            "parenthesized_expression",
                                                            "scoped_identifier",
                                                            "identifier",
                                                            "number_literal",
                                                            "string_literal",
                                                            "boolean_literal"};

/**
 * @brief Checks whether an AST node is an extractable expression.
 * @param[in] node AST node.
 * @return True if candidate node can be extracted into a variable or method.
 */
bool IsExtractableExpression(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    std::string_view type = ts_node_type(node);
    for (std::string_view candidate : kExtractableExpressionTypes)
    {
        if (type == candidate)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks whether an AST node is the left-hand side of an assignment.
 * @param[in] node Candidate expression AST node.
 * @return True if node is on the LHS of an assignment.
 */
bool IsLhsOfAssignment(TSNode node)
{
    TSNode curr = node;
    while (!ts_node_is_null(curr))
    {
        TSNode parent = ts_node_parent(curr);
        if (!ts_node_is_null(parent))
        {
            std::string_view pType = ts_node_type(parent);
            if (pType == "assignment_expression")
            {
                TSNode left = parser::GetChildByField(parent, parser::fields::Left);
                if (ts_node_start_byte(left) == ts_node_start_byte(curr) &&
                    ts_node_end_byte(left) == ts_node_end_byte(curr))
                {
                    return true;
                }
            }
        }
        curr = parent;
    }
    return false;
}

/**
 * @brief Locates the target extractable expression within the requested selection range.
 * @param[in] rootNode Root of AST.
 * @param[in] range Selected text range.
 * @return Valid extractable node or null node if none matches.
 */
TSNode FindExtractableExpression(TSNode rootNode, const lsp::Range& range)
{
    TSPoint startPt = {range.start.line, range.start.character};
    TSPoint endPt = {range.end.line, range.end.character};
    TSNode targetNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    while (!ts_node_is_null(targetNode) && !IsExtractableExpression(targetNode))
    {
        TSNode p = ts_node_parent(targetNode);
        if (ts_node_is_null(p))
        {
            break;
        }
        targetNode = p;
    }
    if (ts_node_is_null(targetNode) || !IsExtractableExpression(targetNode))
    {
        return TSNode{};
    }
    std::string_view nodeType = ts_node_type(targetNode);
    if (nodeType.ends_with("_statement") || nodeType.ends_with("_declaration") || nodeType == "statement_block" ||
        nodeType == "class_body" || nodeType == "parameter" || nodeType == "primitive_type" ||
        IsLhsOfAssignment(targetNode))
    {
        return TSNode{};
    }
    return targetNode;
}

/**
 * @brief Finds the innermost block-level statement enclosing the target node.
 * @param[in] node AST node.
 * @return Enclosing statement AST node.
 */
TSNode FindEnclosingBlockStatement(TSNode node)
{
    TSNode stmtNode = node;
    while (!ts_node_is_null(stmtNode))
    {
        TSNode parent = ts_node_parent(stmtNode);
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "statement_block")
        {
            return stmtNode;
        }
        stmtNode = parent;
    }
    return TSNode{};
}

/**
 * @brief Constructs an Extract Variable refactoring code action.
 * @param[in] request Code action request context.
 * @param[in] targetNode Target expression node.
 * @param[in] stmtNode Statement node above which declaration is inserted.
 * @return Code action if construction succeeds, std::nullopt otherwise.
 */
std::optional<lsp::CodeAction> BuildExtractVariableAction(const CodeActionRequest& request, TSNode targetNode,
                                                          TSNode stmtNode)
{
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    TSPoint exprStart = ts_node_start_point(targetNode);
    TSPoint exprEnd = ts_node_end_point(targetNode);
    const analysis::Scope* scope = FindScopeByLineOrRoot(rootScope.get(), exprStart.row);

    std::string varType =
        analysis::ResolveExpressionType(targetNode, {scope, request.symbolTable, request.sourceCode, request.uri});
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = "newVar";
    std::string exprText = GetNodeText(targetNode, request.sourceCode);
    if (exprText.empty())
    {
        return std::nullopt;
    }

    uint32_t stmtRow = ts_node_start_point(stmtNode).row;
    std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

    lsp::TextEdit declEdit;
    declEdit.range = lsp::Range{{stmtRow, 0}, {stmtRow, 0}};
    declEdit.newText = indent + varType + " " + varName + " = " + exprText + ";\n";

    lsp::TextEdit replEdit;
    replEdit.range = lsp::Range{{exprStart.row, exprStart.column}, {exprEnd.row, exprEnd.column}};
    replEdit.newText = varName;

    lsp::CodeAction action;
    action.title = "Extract Variable";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::RefactorExtract);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(declEdit), std::move(replEdit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    return action;
}

} // namespace

void TryAddExtractVariableAction(const CodeActionRequest& request, TSNode rootNode,
                                 std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }
    TSNode targetNode = FindExtractableExpression(rootNode, request.range);
    if (ts_node_is_null(targetNode))
    {
        return;
    }
    TSNode stmtNode = FindEnclosingBlockStatement(targetNode);
    if (ts_node_is_null(stmtNode))
    {
        return;
    }
    if (auto action = BuildExtractVariableAction(request, targetNode, stmtNode))
    {
        actions.push_back(std::move(*action));
    }
}

} // namespace angel_lsp::features
