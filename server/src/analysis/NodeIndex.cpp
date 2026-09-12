#include "analysis/NodeIndex.h"
#include "analysis/ASTUtils.h"
#include "parser/GrammarNames.h"

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    NodeIndex::NodeIndex(TSNode root, const TSLanguage *lang)
    {
        Build(root, lang);
    }

    void NodeIndex::Build(TSNode root, const TSLanguage *lang)
    {
        Clear();
        m_root = root;
        if (ts_node_is_null(root))
        {
            return;
        }

        if (!lang)
        {
            lang = tree_sitter_angelscript();
        }
        m_language = lang;

        if (lang)
        {
            for (const std::string_view name : parser::k_allNodeTypes)
            {
                const TSSymbol sym = ts_language_symbol_for_name(
                    lang, name.data(), static_cast<uint32_t>(name.length()), true);
                if (sym != 0)
                {
                    m_symbolByName[name] = sym;
                }
            }
        }

        TSTreeCursor cursor = ts_tree_cursor_new(root);
        int depth = 0;
        bool visiting = true;

        while (visiting)
        {
            TSNode node = ts_tree_cursor_current_node(&cursor);
            m_allNodes.push_back(node);

            const TSSymbol sym = ts_node_symbol(node);
            if (sym >= m_nodesBySymbol.size())
            {
                m_nodesBySymbol.resize(static_cast<size_t>(sym) + 1);
            }
            m_nodesBySymbol[sym].push_back(node);

            const char *typeStr = ts_node_type(node);
            if (typeStr)
            {
                m_symbolByName.try_emplace(std::string_view(typeStr), sym);
            }

            if (depth < k_maxAstDepth && ts_tree_cursor_goto_first_child(&cursor))
            {
                depth++;
                continue;
            }

            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                continue;
            }

            bool backtracked = false;
            while (ts_tree_cursor_goto_parent(&cursor))
            {
                depth--;
                if (ts_tree_cursor_goto_next_sibling(&cursor))
                {
                    backtracked = true;
                    break;
                }
            }

            if (!backtracked)
            {
                visiting = false;
            }
        }

        ts_tree_cursor_delete(&cursor);
    }

    std::span<const TSNode> NodeIndex::Nodes(TSSymbol symbol) const noexcept
    {
        if (static_cast<size_t>(symbol) < m_nodesBySymbol.size())
        {
            return m_nodesBySymbol[symbol];
        }
        return {};
    }

    std::span<const TSNode> NodeIndex::Nodes(std::string_view typeName) const noexcept
    {
        const auto it = m_symbolByName.find(typeName);
        if (it != m_symbolByName.end())
        {
            return Nodes(it->second);
        }
        return {};
    }

    TSSymbol NodeIndex::SymbolForName(std::string_view typeName) const noexcept
    {
        const auto it = m_symbolByName.find(typeName);
        if (it != m_symbolByName.end())
        {
            return it->second;
        }
        return 0;
    }

    std::span<const TSNode> NodeIndex::AllNodes() const noexcept
    {
        return m_allNodes;
    }

    TSNode NodeIndex::Root() const noexcept
    {
        return m_root;
    }

    size_t NodeIndex::NodeCount() const noexcept
    {
        return m_allNodes.size();
    }

    bool NodeIndex::Empty() const noexcept
    {
        return m_allNodes.empty();
    }

    void NodeIndex::Clear() noexcept
    {
        m_root = TSNode{};
        m_language = nullptr;
        m_allNodes.clear();
        m_nodesBySymbol.clear();
        m_symbolByName.clear();
    }
}
