#include "analysis/index/CompletionFacts.h"

#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Returns the receiver from the syntax around the dot: the `object` of an enclosing member
 *        expression, or the sibling just before the dot when it is a plain operand without errors.
 * @param[in] rootNode Root of the document tree.
 * @param[in] dotByteOffset Byte offset of the `.`.
 * @return Receiver node, or a null node when the surrounding syntax does not name one.
 */
static TSNode FindReceiverFromParentAtDot(TSNode rootNode, uint32_t dotByteOffset)
{
    TSNode dotNode = ts_node_descendant_for_byte_range(rootNode, dotByteOffset, dotByteOffset + 1);
    if (ts_node_is_null(dotNode))
    {
        return TSNode{};
    }
    TSNode parent = parser::GetParent(dotNode);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(parent) == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(parent, gs.fieldObject);
        if (!ts_node_is_null(obj))
        {
            return obj;
        }
    }
    TSNode prev = ts_node_prev_sibling(dotNode);
    if (!ts_node_is_null(prev))
    {
        const TSSymbol prevSym = ts_node_symbol(prev);
        if (prevSym != gs.symBinaryExpression && prevSym != gs.symUnaryExpression &&
            prevSym != gs.symAssignmentExpression && prevSym != gs.symTernaryExpression && !ts_node_has_error(prev) &&
            !ts_node_is_error(prev))
        {
            return prev;
        }
    }
    return TSNode{};
}

/**
 * @brief Tells whether a node kind ends the upward widening of a receiver expression.
 * @param[in] sym Grammar symbol of the ancestor being considered.
 * @param[in] gs Pre-resolved grammar symbols.
 * @return True for statements, blocks, declarations, operator expressions, argument lists and control flow.
 */
static bool IsExpressionBoundarySymbol(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symScript || sym == gs.symStatementBlock || sym == gs.symExpressionStatement ||
           sym == gs.symVariableDeclaration || sym == gs.symAssignmentExpression || sym == gs.symReturnStatement ||
           sym == gs.symBinaryExpression || sym == gs.symUnaryExpression || sym == gs.symTernaryExpression ||
           sym == gs.symParenthesizedExpression || sym == gs.symArgumentList || sym == gs.symIfStatement ||
           sym == gs.symWhileStatement || sym == gs.symForStatement || sym == gs.symSwitchStatement;
}

/**
 * @brief Recovers the receiver from the text before the dot when the tree did not attach the dot to it.
 *        Starts at the leaf covering the last non-blank byte before the dot and widens to the largest
 *        ancestor that ends before the dot without crossing an expression boundary or an error node.
 * @param[in] rootNode Root of the document tree.
 * @param[in] sourceCode Document text.
 * @param[in] dotByteOffset Byte offset of the `.`.
 * @return Widest receiver node found, or a null node when only blanks precede the dot.
 */
static TSNode FindReceiverPrecedingDot(TSNode rootNode, std::string_view sourceCode, uint32_t dotByteOffset)
{
    size_t b = dotByteOffset;
    while (b > 0 && (sourceCode[b - 1] == ' ' || sourceCode[b - 1] == '\t' || sourceCode[b - 1] == '\r' ||
                     sourceCode[b - 1] == '\n'))
    {
        --b;
    }
    if (b == 0)
    {
        return TSNode{};
    }
    TSNode leaf = ts_node_descendant_for_byte_range(rootNode, static_cast<uint32_t>(b - 1), static_cast<uint32_t>(b));
    if (ts_node_is_null(leaf))
    {
        return TSNode{};
    }
    TSNode curr = leaf;
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachAncestor(parser::GetParent(leaf),
                            [&](TSNode p) -> bool
                            {
                                if (ts_node_is_error(p) || IsExpressionBoundarySymbol(ts_node_symbol(p), gs) ||
                                    ts_node_end_byte(p) > dotByteOffset)
                                {
                                    return false;
                                }
                                curr = p;
                                return true;
                            });
    return curr;
}

std::optional<IndexedByteRange> FindMemberReceiverRange(const NodeIndex& index, std::string_view sourceCode,
                                                        uint32_t dotByteOffset)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode) || dotByteOffset == 0 || dotByteOffset > sourceCode.size())
    {
        return std::nullopt;
    }
    TSNode receiver = FindReceiverFromParentAtDot(rootNode, dotByteOffset);
    if (ts_node_is_null(receiver))
    {
        receiver = FindReceiverPrecedingDot(rootNode, sourceCode, dotByteOffset);
    }
    if (ts_node_is_null(receiver))
    {
        return std::nullopt;
    }
    return IndexedByteRange{ts_node_start_byte(receiver), ts_node_end_byte(receiver)};
}

bool IsInClassBodyOutsideCallable(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return false;
    }
    TSPoint pt{line, column};
    TSNode cur = ts_node_descendant_for_point_range(rootNode, pt, pt);
    const auto& gs = parser::GetGrammarSymbols();
    bool inClass = false;
    parser::ForEachAncestor(cur,
                            [&](TSNode node) -> bool
                            {
                                const TSSymbol sym = ts_node_symbol(node);
                                if (sym == gs.symFuncDeclaration || sym == gs.symStatementBlock)
                                {
                                    return false;
                                }
                                if (sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration)
                                {
                                    inClass = true;
                                    return false;
                                }
                                return true;
                            });
    return inClass;
}

} // namespace angel_lsp::analysis
