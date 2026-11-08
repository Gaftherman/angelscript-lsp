#include "analysis/index/FormattingFacts.h"

#include "document/Document.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/SyntaxTree.h"

#include <array>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

bool IsOnTypeFormattingSuppressed(const NodeIndex& index, uint32_t line, uint32_t column)
{
    TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return false;
    }
    if (ts_node_has_error(root))
    {
        return true;
    }
    TSPoint pt{line, column};
    TSNode nodeAtPoint = ts_node_descendant_for_point_range(root, pt, pt);
    if (ts_node_is_null(nodeAtPoint))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> skipSyms = {gs.symStringLiteral, gs.symComment};
    return !ts_node_is_null(parser::FindAncestorWithAnySymbol(nodeAtPoint, skipSyms));
}

/**
 * @brief Captures the byte layout of one top-level declaration, reading the namespace fields when present.
 * @param[in] child Named top-level child of the stub root.
 * @param[in] gs Grammar symbols and field ids.
 * @return Declaration layout; name and body bytes stay 0 unless the namespace has both fields.
 */
static IndexedStubDeclaration MakeStubDeclaration(TSNode child, const parser::GrammarSymbols& gs)
{
    IndexedStubDeclaration decl;
    decl.startByte = ts_node_start_byte(child);
    decl.endByte = ts_node_end_byte(child);
    decl.isNamespace = ts_node_symbol(child) == gs.symNamespaceDeclaration;
    if (!decl.isNamespace)
    {
        return decl;
    }
    const TSNode nameNode = ts_node_child_by_field_id(child, gs.fieldName);
    const TSNode bodyNode = ts_node_child_by_field_id(child, gs.fieldBody);
    if (ts_node_is_null(nameNode) || ts_node_is_null(bodyNode))
    {
        return decl;
    }
    decl.hasNameAndBody = true;
    decl.nameStartByte = ts_node_start_byte(nameNode);
    decl.nameEndByte = ts_node_end_byte(nameNode);
    decl.bodyStartByte = ts_node_start_byte(bodyNode);
    decl.bodyEndByte = ts_node_end_byte(bodyNode);
    return decl;
}

std::optional<std::vector<IndexedStubDeclaration>> ExtractStubDeclarations(std::string_view source,
                                                                           parser::AngelScriptParser& parser)
{
    parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(source));
    if (tree == nullptr)
    {
        return std::nullopt;
    }
    const TSNode root = ts_tree_root_node(tree.get());
    if (ts_node_has_error(root))
    {
        return std::nullopt;
    }

    std::vector<IndexedStubDeclaration> declarations;
    const auto& gs = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    for (bool more = ts_tree_cursor_goto_first_child(&cursor); more; more = ts_tree_cursor_goto_next_sibling(&cursor))
    {
        const TSNode child = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_named(child) && ts_node_symbol(child) != gs.symComment)
        {
            declarations.push_back(MakeStubDeclaration(child, gs));
        }
    }
    ts_tree_cursor_delete(&cursor);
    return declarations;
}

} // namespace angel_lsp::analysis
