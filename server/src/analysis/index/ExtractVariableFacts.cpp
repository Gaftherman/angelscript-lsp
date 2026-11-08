#include "analysis/index/ExtractVariableFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <array>
#include <optional>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether an AST node is an extractable expression.
 * @param[in] node AST node.
 * @return True if candidate node can be extracted into a variable or method.
 */
static bool IsExtractableExpression(TSNode node)
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
static bool IsLhsOfAssignment(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode assign = parser::FindAncestorWithSymbol(node, gs.symAssignmentExpression);
    if (ts_node_is_null(assign))
    {
        return false;
    }
    TSNode left = parser::GetChildByField(assign, gs.fieldLeft);
    return !ts_node_is_null(left) && ts_node_start_byte(node) >= ts_node_start_byte(left) &&
           ts_node_end_byte(node) <= ts_node_end_byte(left);
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

/**
 * @brief Checks whether a grammar symbol represents a statement or container that cannot be extracted.
 * @param[in] nodeSym Grammar symbol to inspect.
 * @param[in] gs Precompiled grammar symbols table.
 * @return True if node symbol is a non-extractable statement or container.
 */
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
static TSNode FindExtractableExpression(TSNode rootNode, const SourceRange& range)
{
    TSPoint startPt = {range.startLine, range.startCharacter};
    TSPoint endPt = {range.endLine, range.endCharacter};
    TSNode startNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    TSNode targetNode{};
    parser::ForEachAncestor(startNode,
                            [&](TSNode cur) -> bool
                            {
                                if (IsExtractableExpression(cur))
                                {
                                    targetNode = cur;
                                    return false;
                                }
                                return true;
                            });
    if (ts_node_is_null(targetNode))
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
static TSNode FindEnclosingBlockStatement(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    return parser::FindDirectChildOfAncestor(node, gs.symStatementBlock);
}

std::optional<IndexedExtractVariableSite> FindExtractVariableSite(const NodeIndex& index, const SourceRange& selection)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const TSNode targetNode = FindExtractableExpression(rootNode, selection);
    if (ts_node_is_null(targetNode))
    {
        return std::nullopt;
    }

    const TSNode stmtNode = FindEnclosingBlockStatement(targetNode);
    if (ts_node_is_null(stmtNode))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSPoint targetStart = ts_node_start_point(targetNode);
    const TSPoint targetEnd = ts_node_end_point(targetNode);
    const TSPoint stmtStart = ts_node_start_point(stmtNode);
    const TSNode parentBlock = parser::GetParent(stmtNode);

    IndexedExtractVariableSite site;
    site.expression = IndexedByteRange{ts_node_start_byte(targetNode), ts_node_end_byte(targetNode)};
    site.expressionRange = SourceRange{targetStart.row, targetStart.column, targetEnd.row, targetEnd.column};
    site.statementLine = stmtStart.row;
    site.statementColumn = stmtStart.column;
    site.statementSharesBlockLine = !ts_node_is_null(parentBlock) &&
                                    ts_node_symbol(parentBlock) == gs.symStatementBlock &&
                                    ts_node_start_point(parentBlock).row == stmtStart.row;

    const TSSymbol targetSym = ts_node_symbol(targetNode);
    if (targetSym == gs.symCallExpression)
    {
        const TSNode funcNode = parser::GetChildByField(targetNode, gs.fieldFunction);
        if (!ts_node_is_null(funcNode))
        {
            site.hintKind = ExtractNameHintKind::CallCallee;
            if (ts_node_symbol(funcNode) == gs.symMemberExpression)
            {
                const TSNode memberNode = parser::GetChildByField(funcNode, gs.fieldMember);
                if (!ts_node_is_null(memberNode))
                {
                    site.hintName = IndexedByteRange{ts_node_start_byte(memberNode), ts_node_end_byte(memberNode)};
                }
            }
            else
            {
                site.hintName = IndexedByteRange{ts_node_start_byte(funcNode), ts_node_end_byte(funcNode)};
            }
        }
    }
    else if (targetSym == gs.symMemberExpression)
    {
        site.hintKind = ExtractNameHintKind::Member;
        const TSNode memberNode = parser::GetChildByField(targetNode, gs.fieldMember);
        if (!ts_node_is_null(memberNode))
        {
            site.hintName = IndexedByteRange{ts_node_start_byte(memberNode), ts_node_end_byte(memberNode)};
        }
    }
    else if (targetSym == gs.symConstructCallExpression)
    {
        site.hintKind = ExtractNameHintKind::ConstructType;
        const TSNode typeNode = parser::GetChildByField(targetNode, gs.fieldType);
        if (!ts_node_is_null(typeNode))
        {
            site.hintName = IndexedByteRange{ts_node_start_byte(typeNode), ts_node_end_byte(typeNode)};
        }
    }

    return site;
}

} // namespace angel_lsp::analysis
