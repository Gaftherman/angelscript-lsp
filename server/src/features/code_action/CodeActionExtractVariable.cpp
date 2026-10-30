/**
 * @file CodeActionExtractVariable.cpp
 * @brief Refactoring provider for Extract Variable code action.
 */

#include "features/code_action/CodeActionInternal.h"
#include "parser/ASTUtils.h"
#include "parser/Keywords.h"

namespace angel_lsp::features
{
namespace
{

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
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    static const auto kSymbols = [&gs]()
    {
        return std::array<TSSymbol, 16>{gs.symBinaryExpression,
                                        gs.symUnaryExpression,
                                        gs.symPostfixExpression,
                                        gs.symCallExpression,
                                        gs.symMemberExpression,
                                        gs.symTernaryExpression,
                                        gs.symCastExpression,
                                        gs.symFunctionalCastExpression,
                                        gs.symConstructCallExpression,
                                        gs.symIndexExpression,
                                        gs.symParenthesizedExpression,
                                        gs.symScopedIdentifier,
                                        gs.symIdentifier,
                                        gs.symNumberLiteral,
                                        gs.symStringLiteral,
                                        gs.symBooleanLiteral};
    }();
    return std::find(kSymbols.begin(), kSymbols.end(), sym) != kSymbols.end();
}

/**
 * @brief Checks whether an AST node is the left-hand side of an assignment.
 * @param[in] node Candidate expression AST node.
 * @return True if node is on the LHS of an assignment.
 */
bool IsLhsOfAssignment(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    int depth = 0;
    for (TSNode curr = node; depth < 64 && !ts_node_is_null(curr); ++depth, curr = parser::GetParent(curr))
    {
        TSNode parent = parser::GetParent(curr);
        if (!ts_node_is_null(parent))
        {
            if (ts_node_symbol(parent) == gs.symAssignmentExpression)
            {
                TSNode left = parser::GetChildByField(parent, gs.fieldLeft);
                if (ts_node_start_byte(left) == ts_node_start_byte(curr) &&
                    ts_node_end_byte(left) == ts_node_end_byte(curr))
                {
                    return true;
                }
            }
        }
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
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symMemberExpression)
    {
        TSNode memberNode = parser::GetChildByField(parent, gs.fieldMember);
        return ts_node_eq(memberNode, node) ||
               (!ts_node_is_null(memberNode) && ts_node_start_byte(memberNode) == ts_node_start_byte(node));
    }
    return pType == gs.symScopedIdentifier;
}

/**
 * @brief Checks if a node is the function callee of a call_expression.
 * @param[in] node Candidate node.
 * @param[in] parent Parent node.
 * @return True if node is the callee of a call.
 */
static bool IsFunctionFieldOfCall(TSNode node, TSNode parent)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != gs.symCallExpression)
    {
        return false;
    }
    TSNode funcNode = parser::GetChildByField(parent, gs.fieldFunction);
    return ts_node_eq(funcNode, node) ||
           (!ts_node_is_null(funcNode) && ts_node_start_byte(funcNode) == ts_node_start_byte(node));
}

static bool IsNonExtractableStatementOrContainer(TSSymbol nodeSym, const parser::GrammarSymbols& gs) noexcept
{
    const std::array<TSSymbol, 13> kExcluded = {
        gs.symStatementBlock,      gs.symClassBody,           gs.symParameter,       gs.symPrimitiveType,
        gs.symExpressionStatement, gs.symReturnStatement,     gs.symIfStatement,     gs.symWhileStatement,
        gs.symForStatement,        gs.symVariableDeclaration, gs.symFuncDeclaration, gs.symClassDeclaration,
        gs.symInterfaceDeclaration};
    return std::find(kExcluded.begin(), kExcluded.end(), nodeSym) != kExcluded.end();
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
        targetNode = parser::GetParent(targetNode);
    }
    if (ts_node_is_null(targetNode) || !IsExtractableExpression(targetNode))
    {
        return TSNode{};
    }

    TSNode parent = parser::GetParent(targetNode);
    if (IsMemberField(targetNode, parent))
    {
        targetNode = parent;
        parent = parser::GetParent(targetNode);
    }
    if (IsFunctionFieldOfCall(targetNode, parent))
    {
        targetNode = parent;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol nodeSym = ts_node_symbol(targetNode);
    if (IsNonExtractableStatementOrContainer(nodeSym, gs) || IsLhsOfAssignment(targetNode))
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
    const auto& gs = parser::GetGrammarSymbols();
    int depth = 0;
    for (TSNode stmtNode = node; depth < 64 && !ts_node_is_null(stmtNode);
         ++depth, stmtNode = parser::GetParent(stmtNode))
    {
        TSNode parent = parser::GetParent(stmtNode);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symStatementBlock)
        {
            return stmtNode;
        }
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
    const auto& gs = parser::GetGrammarSymbols();
    TSNode funcNode = parser::GetChildByField(targetNode, gs.fieldFunction);
    if (ts_node_is_null(funcNode))
    {
        return "";
    }
    std::string fnNameText;
    if (ts_node_symbol(funcNode) == gs.symMemberExpression)
    {
        TSNode nameNode = parser::GetChildByField(funcNode, gs.fieldMember);
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
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(targetNode);
    if (type == gs.symMemberExpression)
    {
        TSNode nameNode = parser::GetChildByField(targetNode, gs.fieldMember);
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
    if (type == gs.symConstructCallExpression)
    {
        TSNode typeNode = parser::GetChildByField(targetNode, gs.fieldType);
        std::string tText = GetNodeText(typeNode, sourceCode);
        if (!tText.empty())
        {
            return ToLowerFirst(tText);
        }
    }
    return "";
}

std::string SuggestCandidateName(TSNode targetNode, std::string_view varType, std::string_view sourceCode,
                                 const analysis::SymbolTable* symbolTable)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol type = ts_node_symbol(targetNode);
    if (type == gs.symCallExpression)
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
    const std::string_view strType = symbolTable ? symbolTable->GetStringType() : std::string_view("string");
    if (!varType.empty() && varType != parser::keywords::Auto && varType != parser::keywords::Null &&
        !analysis::IsPrimitiveTypeName(varType))
    {
        std::string clean = analysis::CleanBaseType(varType);
        if (!clean.empty() && clean != strType)
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
                                const analysis::Scope* scope, const analysis::SymbolTable* symbolTable)
{
    std::string candidate = SuggestCandidateName(targetNode, varType, sourceCode, symbolTable);
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
    if (analysis::CleanBaseTypeView(varType) == "void")
    {
        return std::nullopt;
    }
    if (varType.empty() || varType == "null")
    {
        varType = "auto";
    }

    std::string varName = SuggestVariableName(targetNode, varType, request.sourceCode, scope, &request.symbolTable);
    std::string exprText = GetNodeText(targetNode, request.sourceCode);
    if (exprText.empty())
    {
        return std::nullopt;
    }

    uint32_t stmtRow = ts_node_start_point(stmtNode).row;
    std::string indent = GetLineIndentation(request.sourceCode, stmtRow);

    lsp::TextEdit declEdit;
    TSNode parentBlock = parser::GetParent(stmtNode);
    TSPoint stmtStart = ts_node_start_point(stmtNode);
    if (!ts_node_is_null(parentBlock) && ts_node_symbol(parentBlock) == parser::GetGrammarSymbols().symStatementBlock &&
        ts_node_start_point(parentBlock).row == stmtStart.row)
    {
        declEdit.range = lsp::Range{{stmtStart.row, stmtStart.column}, {stmtStart.row, stmtStart.column}};
        declEdit.newText = varType + " " + varName + " = " + exprText + "; ";
    }
    else
    {
        declEdit.range = lsp::Range{{stmtRow, 0}, {stmtRow, 0}};
        declEdit.newText = indent + varType + " " + varName + " = " + exprText + ";\n";
    }

    lsp::TextEdit replEdit;
    replEdit.range = lsp::Range{{exprStart.row, exprStart.column}, {exprEnd.row, exprEnd.column}};
    replEdit.newText = varName;

    lsp::CodeAction action;
    action.title = i18n::FormatMessage(request.i18n, "action-extract-variable", "Extract Variable");
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
