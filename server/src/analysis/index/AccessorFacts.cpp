#include "analysis/index/AccessorFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the insertion point for the 'property' keyword, placing it after 'const' if present.
 * @param[in] funcNode The func_declaration AST node.
 * @param[in] parametersNode The parameter list AST node.
 * @return Insertion point coordinates.
 */
static TSPoint FindAccessorPropertyInsertionPoint(TSNode funcNode, TSNode parametersNode)
{
    TSPoint insertAt = ts_node_end_point(parametersNode);
    const uint32_t paramsEndByte = ts_node_end_byte(parametersNode);
    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_null(child) && ts_node_start_byte(child) >= paramsEndByte)
            {
                if (ts_node_symbol(child) == gs.tokConst)
                {
                    insertAt = ts_node_end_point(child);
                    break;
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return insertAt;
}

std::optional<IndexedAccessorSite> FindAccessorSite(const NodeIndex& index, std::string_view sourceCode, uint32_t line,
                                                    uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const TSPoint pt{line, column};
    const TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode classNode = parser::FindAncestorWithSymbol(leaf, gs.symClassDeclaration);
    if (ts_node_is_null(classNode))
    {
        return std::nullopt;
    }

    const TSNode classBody = FindClassBodyNode(classNode);
    if (ts_node_is_null(classBody))
    {
        return std::nullopt;
    }

    const TSNode nameNode = parser::GetChildByField(classNode, gs.fieldName);
    const IndexedByteRange classNameRange =
        ts_node_is_null(nameNode) ? IndexedByteRange{0, 0}
                                  : IndexedByteRange{ts_node_start_byte(nameNode), ts_node_end_byte(nameNode)};

    const TSNode varDecl = parser::FindAncestorWithSymbol(leaf, gs.symVariableDeclaration);
    if (ts_node_is_null(varDecl) || ts_node_start_byte(varDecl) < ts_node_start_byte(classBody) ||
        ts_node_end_byte(varDecl) > ts_node_end_byte(classBody))
    {
        return std::nullopt;
    }

    TSNode typeNode = parser::GetChildByField(varDecl, gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(varDecl, gs.fieldType);
    }
    const IndexedByteRange fieldTypeRange =
        ts_node_is_null(typeNode) ? IndexedByteRange{0, 0}
                                  : IndexedByteRange{ts_node_start_byte(typeNode), ts_node_end_byte(typeNode)};

    std::vector<IndexedByteRange> fieldNames;
    TSTreeCursor cursor = ts_tree_cursor_new(varDecl);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            const TSNode ch = ts_tree_cursor_current_node(&cursor);
            if (!ts_node_is_null(ch) && ts_node_symbol(ch) == gs.symVariableDeclarator)
            {
                const TSNode vNameNode = parser::GetChildByField(ch, gs.fieldName);
                if (!ts_node_is_null(vNameNode) && !parser::NodeText(vNameNode, sourceCode).empty())
                {
                    fieldNames.push_back(IndexedByteRange{ts_node_start_byte(vNameNode), ts_node_end_byte(vNameNode)});
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);

    if (fieldNames.empty())
    {
        return std::nullopt;
    }

    const TSPoint closingPt = FindClassClosingBracePoint(classBody);
    const IndexedSourcePoint insertPoint{closingPt.row, closingPt.column};

    return IndexedAccessorSite{
        classNameRange,
        insertPoint,
        fieldTypeRange,
        std::move(fieldNames),
    };
}

std::optional<IndexedSourcePoint> FindPropertyKeywordInsertionPoint(const NodeIndex& index, uint32_t line,
                                                                    uint32_t column)
{
    const TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSPoint point{line, column};
    const TSNode node = parser::FindEnclosingNodeAtPoint(root, point, gs.symFuncDeclaration);
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    const TSNode parameters = parser::GetChildByField(node, gs.fieldParameters);
    if (ts_node_is_null(parameters))
    {
        return std::nullopt;
    }

    const TSPoint insertAt = FindAccessorPropertyInsertionPoint(node, parameters);
    return IndexedSourcePoint{insertAt.row, insertAt.column};
}

} // namespace angel_lsp::analysis
