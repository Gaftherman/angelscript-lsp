#include "analysis/index/ClassMutationFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a node type represents a method or constructor declaration that should not be recursed into.
 * @param[in] nodeType Tree-sitter node type.
 * @return True if subtree should be skipped for field collection.
 */
static bool IsSubtreeSkippable(TSSymbol sym, const parser::GrammarSymbols& gs) noexcept
{
    return sym == gs.symFuncDeclaration;
}

/**
 * @brief Traverses up the AST cursor to find the next available sibling or reaches root.
 * @param[in,out] cursor Active tree-sitter cursor.
 * @param[in] rootNode Enclosing root node boundary.
 * @return True if a next sibling was found, false if root was reached.
 */
static bool StepUpCursor(TSTreeCursor& cursor, TSNode rootNode)
{
    while (true)
    {
        if (!ts_tree_cursor_goto_parent(&cursor))
        {
            return false;
        }
        if (ts_node_eq(ts_tree_cursor_current_node(&cursor), rootNode))
        {
            return false;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            return true;
        }
    }
}

/**
 * @brief Collects field declarator names from a class AST node using a flat cursor walk.
 * @param[in] classNode Class declaration AST node.
 * @param[in] sourceCode Document source text.
 * @param[in,out] facts Mutation facts structure to populate.
 */
static void CollectAstClassFields(TSNode classNode, std::string_view sourceCode, IndexedMethodMutationFacts& facts)
{
    if (ts_node_is_null(classNode))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(classNode);

    const auto& gs = parser::GetGrammarSymbols();
    while (true)
    {
        TSNode curr = ts_tree_cursor_current_node(&cursor);
        const TSSymbol sym = ts_node_is_null(curr) ? static_cast<TSSymbol>(0) : ts_node_symbol(curr);

        if (sym == gs.symVariableDeclarator)
        {
            TSNode vNameNode = parser::GetChildByField(curr, gs.fieldName);
            if (!parser::NodeText(vNameNode, sourceCode).empty())
            {
                facts.classFieldNames.push_back(
                    IndexedByteRange{ts_node_start_byte(vNameNode), ts_node_end_byte(vNameNode)});
            }
        }

        if (!IsSubtreeSkippable(sym, gs) && ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, classNode))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

/**
 * @brief Records a mutation site if the AST node is an assignment expression targeting a class field or this member.
 * @param[in] curr Candidate assignment AST node.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination vector for recorded mutation sites.
 * @return True if a candidate mutation site was recorded.
 */
static bool RecordAssignmentMutationSite(TSNode curr, const parser::GrammarSymbols& gs,
                                         std::vector<IndexedMutationSite>& sites)
{
    if (ts_node_symbol(curr) != gs.symAssignmentExpression)
    {
        return false;
    }
    TSNode left = parser::GetChildByField(curr, gs.fieldLeft);
    if (ts_node_is_null(left))
    {
        return false;
    }
    const TSSymbol lSym = ts_node_symbol(left);
    if (lSym == gs.symIdentifier || lSym == gs.symScopedIdentifier)
    {
        sites.push_back(IndexedMutationSite{
            MutationSiteKind::NamedTarget,
            IndexedByteRange{ts_node_start_byte(left), ts_node_end_byte(left)},
            ts_node_start_point(left).row,
        });
        return true;
    }
    if (lSym == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(left, gs.fieldObject);
        TSNode mem = parser::GetChildByField(left, gs.fieldMember);
        if (!ts_node_is_null(obj) && ts_node_symbol(obj) == gs.symThisExpression)
        {
            const IndexedByteRange memRange = ts_node_is_null(mem)
                                                  ? IndexedByteRange{0, 0}
                                                  : IndexedByteRange{ts_node_start_byte(mem), ts_node_end_byte(mem)};
            sites.push_back(IndexedMutationSite{
                MutationSiteKind::ThisMember,
                memRange,
                ts_node_start_point(left).row,
            });
            return true;
        }
    }
    return false;
}

/**
 * @brief Records a mutation site if the AST node is an increment or decrement expression.
 * @param[in] curr Candidate increment or decrement AST node.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination vector for recorded mutation sites.
 * @return True if a candidate mutation site was recorded.
 */
static bool RecordIncDecMutationSite(TSNode curr, const parser::GrammarSymbols& gs,
                                     std::vector<IndexedMutationSite>& sites)
{
    const TSSymbol type = ts_node_symbol(curr);
    if (type != gs.symPostfixExpression && type != gs.symUnaryExpression)
    {
        return false;
    }
    TSNode opNode = parser::GetChildByField(curr, gs.fieldOperator);
    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym != gs.tokPlusPlus && opSym != gs.tokDashDash)
    {
        return false;
    }
    TSNode arg = parser::GetChildByField(curr, gs.fieldOperand);
    if (ts_node_is_null(arg))
    {
        return false;
    }
    const TSSymbol aType = ts_node_symbol(arg);
    if (aType == gs.symIdentifier || aType == gs.symScopedIdentifier)
    {
        sites.push_back(IndexedMutationSite{
            MutationSiteKind::NamedTarget,
            IndexedByteRange{ts_node_start_byte(arg), ts_node_end_byte(arg)},
            ts_node_start_point(arg).row,
        });
        return true;
    }
    return false;
}

/**
 * @brief Records a mutation site if the AST node is a method call on this.
 * @param[in] curr Candidate call expression AST node.
 * @param[in] gs Precompiled grammar symbols.
 * @param[in,out] sites Destination vector for recorded mutation sites.
 * @return True if a candidate mutation site was recorded.
 */
static bool RecordCallMutationSite(TSNode curr, const parser::GrammarSymbols& gs,
                                   std::vector<IndexedMutationSite>& sites)
{
    if (ts_node_symbol(curr) != gs.symCallExpression)
    {
        return false;
    }
    TSNode callee = parser::GetChildByField(curr, gs.fieldFunction);
    if (ts_node_is_null(callee))
    {
        return false;
    }
    const TSSymbol cType = ts_node_symbol(callee);
    if (cType == gs.symIdentifier || cType == gs.symScopedIdentifier)
    {
        sites.push_back(IndexedMutationSite{
            MutationSiteKind::ThisCall,
            IndexedByteRange{ts_node_start_byte(callee), ts_node_end_byte(callee)},
            ts_node_start_point(callee).row,
        });
        return true;
    }
    if (cType == gs.symMemberExpression)
    {
        TSNode obj = parser::GetChildByField(callee, gs.fieldObject);
        TSNode mem = parser::GetChildByField(callee, gs.fieldMember);
        if (!ts_node_is_null(obj) && ts_node_symbol(obj) == gs.symThisExpression)
        {
            const IndexedByteRange memRange = ts_node_is_null(mem)
                                                  ? IndexedByteRange{0, 0}
                                                  : IndexedByteRange{ts_node_start_byte(mem), ts_node_end_byte(mem)};
            sites.push_back(IndexedMutationSite{
                MutationSiteKind::ThisCall,
                memRange,
                ts_node_start_point(callee).row,
            });
            return true;
        }
    }
    return false;
}

void CollectMethodMutationFacts(TSNode bodyNode, TSNode classNode, std::string_view sourceCode,
                                IndexedMethodMutationFacts& facts)
{
    facts = {};
    if (ts_node_is_null(bodyNode))
    {
        return;
    }

    CollectAstClassFields(classNode, sourceCode, facts);

    facts.bodyStartLine = ts_node_start_point(bodyNode).row;
    facts.bodyEndLine = ts_node_end_point(bodyNode).row;

    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(bodyNode);

    while (true)
    {
        TSNode curr = ts_tree_cursor_current_node(&cursor);
        if (!ts_node_is_null(curr))
        {
            if (!RecordAssignmentMutationSite(curr, gs, facts.sites))
            {
                if (!RecordIncDecMutationSite(curr, gs, facts.sites))
                {
                    RecordCallMutationSite(curr, gs, facts.sites);
                }
            }
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }
        if (!StepUpCursor(cursor, bodyNode))
        {
            break;
        }
    }

    ts_tree_cursor_delete(&cursor);
}

} // namespace angel_lsp::analysis
