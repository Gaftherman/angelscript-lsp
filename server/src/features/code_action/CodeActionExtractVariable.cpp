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
 * @brief Checks if a node is the member field of a member_expression or part of a scoped_identifier.
 * @param[in] node Candidate node.
 * @param[in] parent Parent node.
 * @return True if node is a member component without standalone evaluation semantics.
 */
static bool IsMemberField(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent))
    {
        return false;
    }
    std::string_view pType = ts_node_type(parent);
    if (pType == "member_expression")
    {
        TSNode memberNode = parser::GetChildByField(parent, parser::fields::Member);
        return ts_node_eq(memberNode, node) ||
               (!ts_node_is_null(memberNode) && ts_node_start_byte(memberNode) == ts_node_start_byte(node));
    }
    return pType == "scoped_identifier";
}

/**
 * @brief Checks if a node is the function callee of a call_expression.
 * @param[in] node Candidate node.
 * @param[in] parent Parent node.
 * @return True if node is the callee of a call.
 */
static bool IsFunctionFieldOfCall(TSNode node, TSNode parent)
{
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "call_expression")
    {
        return false;
    }
    TSNode funcNode = parser::GetChildByField(parent, parser::fields::Function);
    return ts_node_eq(funcNode, node) ||
           (!ts_node_is_null(funcNode) && ts_node_start_byte(funcNode) == ts_node_start_byte(node));
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

    TSNode parent = ts_node_parent(targetNode);
    if (IsMemberField(targetNode, parent))
    {
        targetNode = parent;
        parent = ts_node_parent(targetNode);
    }
    if (IsFunctionFieldOfCall(targetNode, parent))
    {
        targetNode = parent;
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

std::string ToLowerFirst(std::string_view str)
{
    if (str.empty())
    {
        return "newVar";
    }
    std::string result(str);
    result[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[0])));
    return result;
}

std::string SuggestNameFromCall(TSNode targetNode, std::string_view sourceCode)
{
    TSNode funcNode = parser::GetChildByField(targetNode, parser::fields::Function);
    if (ts_node_is_null(funcNode))
    {
        return "";
    }
    std::string fnNameText;
    if (std::string_view(ts_node_type(funcNode)) == "member_expression")
    {
        TSNode nameNode = parser::GetChildByField(funcNode, parser::fields::Member);
        fnNameText = GetNodeText(nameNode, sourceCode);
    }
    else
    {
        fnNameText = GetNodeText(funcNode, sourceCode);
    }
    if (fnNameText.starts_with("Get") && fnNameText.size() > 3)
    {
        return ToLowerFirst(fnNameText.substr(3));
    }
    if (fnNameText.starts_with("get_") && fnNameText.size() > 4)
    {
        return ToLowerFirst(fnNameText.substr(4));
    }
    if (fnNameText.starts_with("Find") && fnNameText.size() > 4)
    {
        return ToLowerFirst(fnNameText.substr(4));
    }
    if (fnNameText.starts_with("Create") && fnNameText.size() > 6)
    {
        return ToLowerFirst(fnNameText.substr(6));
    }
    return "";
}

std::string SuggestNameFromMemberOrConstruct(TSNode targetNode, std::string_view sourceCode)
{
    std::string_view type = ts_node_type(targetNode);
    if (type == "member_expression")
    {
        TSNode nameNode = parser::GetChildByField(targetNode, parser::fields::Member);
        std::string memText = GetNodeText(nameNode, sourceCode);
        if (memText.starts_with("m_") && memText.size() > 2)
        {
            return ToLowerFirst(memText.substr(2));
        }
        if (!memText.empty())
        {
            return ToLowerFirst(memText);
        }
    }
    if (type == "construct_call_expression")
    {
        TSNode typeNode = parser::GetChildByField(targetNode, parser::fields::Type);
        std::string tText = GetNodeText(typeNode, sourceCode);
        if (!tText.empty())
        {
            return ToLowerFirst(tText);
        }
    }
    return "";
}

std::string SuggestCandidateName(TSNode targetNode, std::string_view varType, std::string_view sourceCode)
{
    std::string_view type = ts_node_type(targetNode);
    if (type == "call_expression")
    {
        if (std::string callName = SuggestNameFromCall(targetNode, sourceCode); !callName.empty())
        {
            return callName;
        }
    }
    if (std::string memName = SuggestNameFromMemberOrConstruct(targetNode, sourceCode); !memName.empty())
    {
        return memName;
    }
    if (!varType.empty() && varType != "auto" && varType != "null" && varType != "int" &&
        varType != "uint" && varType != "float" && varType != "double" && varType != "bool")
    {
        std::string clean = analysis::CleanBaseType(varType);
        if (!clean.empty() && clean != "string")
        {
            return ToLowerFirst(clean);
        }
    }
    return "newVar";
}

bool IsNameInScope(const analysis::Scope* scope, std::string_view name)
{
    const analysis::Scope* curr = scope;
    while (curr)
    {
        for (const auto& def : curr->definitions)
        {
            if (def.name == name)
            {
                return true;
            }
        }
        curr = curr->parent;
    }
    return false;
}

std::string SuggestVariableName(TSNode targetNode, std::string_view varType, std::string_view sourceCode,
                                const analysis::Scope* scope)
{
    std::string candidate = SuggestCandidateName(targetNode, varType, sourceCode);
    if (!scope)
    {
        return candidate;
    }
    std::string result = candidate;
    int suffix = 1;
    while (IsNameInScope(scope, result))
    {
        result = candidate + std::to_string(suffix++);
    }
    return result;
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
    if (analysis::CleanBaseType(varType) == "void")
    {
        return std::nullopt;
    }
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = SuggestVariableName(targetNode, varType, request.sourceCode, scope);
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
