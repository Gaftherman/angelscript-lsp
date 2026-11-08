#include "parser/DoxygenSyntax.h"

#include "parser/DoxygenParser.h"
#include "parser/SyntaxTree.h"

#include <cstdint>
#include <cstring>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::parser
{

/**
 * @brief Maps a tree-sitter-doxygen grammar type string to its corresponding DoxygenNodeKind.
 * @param[in] type Grammar node type string returned by tree-sitter.
 * @return The mapped DoxygenNodeKind, or DoxygenNodeKind::Other if not recognized.
 */
static DoxygenNodeKind MapNodeTypeToKind(const char* type)
{
    if (type == nullptr)
    {
        return DoxygenNodeKind::Other;
    }

    static constexpr std::pair<const char*, DoxygenNodeKind> k_typeTable[] = {
        {"brief_header", DoxygenNodeKind::BriefHeader},
        {"brief_description", DoxygenNodeKind::BriefDescription},
        {"description", DoxygenNodeKind::Description},
        {"code_block", DoxygenNodeKind::CodeBlock},
        {"code_block_language", DoxygenNodeKind::CodeBlockLanguage},
        {"code_block_content", DoxygenNodeKind::CodeBlockContent},
        {"tag", DoxygenNodeKind::Tag},
        {"tag_name", DoxygenNodeKind::TagName},
        {"storageclass", DoxygenNodeKind::StorageClass},
        {"identifier", DoxygenNodeKind::Identifier},
        {"qualified_identifier", DoxygenNodeKind::QualifiedIdentifier},
        {"function", DoxygenNodeKind::Function},
        {"function_link", DoxygenNodeKind::FunctionLink},
        {"type", DoxygenNodeKind::Type},
        {"_text_line", DoxygenNodeKind::TextLine},
        {"code_word", DoxygenNodeKind::CodeWord},
        {"code", DoxygenNodeKind::Code},
        {"emphasis", DoxygenNodeKind::Emphasis},
        {"text", DoxygenNodeKind::Text},
        {"link", DoxygenNodeKind::Link}};

    for (const auto& entry : k_typeTable)
    {
        if (std::strcmp(type, entry.first) == 0)
        {
            return entry.second;
        }
    }

    return DoxygenNodeKind::Other;
}

DoxygenSyntaxFacts ExtractDoxygenSyntax(std::string_view text)
{
    if (text.empty() || text.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos)
    {
        return {};
    }

    DoxygenParser p;
    parser::TreePtr tree = parser::MakeTreePtr(p.Parse(text));
    if (!tree)
    {
        return {};
    }

    TSNode rootNode = ts_tree_root_node(tree.get());
    if (ts_node_is_null(rootNode))
    {
        return {};
    }

    DoxygenSyntaxFacts facts;
    facts.nodes.push_back(
        DoxygenNodeFact{DoxygenNodeKind::Root, true, ts_node_start_byte(rootNode), ts_node_end_byte(rootNode), 0, 0});

    std::vector<TSNode> pending;
    pending.push_back(rootNode);

    uint32_t head = 0;
    while (head < facts.nodes.size())
    {
        facts.nodes[head].firstChild = static_cast<uint32_t>(facts.nodes.size());
        uint32_t count = 0;

        TSTreeCursor cursor = ts_tree_cursor_new(pending[head]);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode c = ts_tree_cursor_current_node(&cursor);
                DoxygenNodeKind kind = MapNodeTypeToKind(ts_node_type(c));
                facts.nodes.push_back(
                    DoxygenNodeFact{kind, ts_node_is_named(c), ts_node_start_byte(c), ts_node_end_byte(c), 0, 0});
                pending.push_back(c);
                ++count;
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);

        facts.nodes[head].childCount = count;
        ++head;
    }

    return facts;
}

} // namespace angel_lsp::parser
