#include "analysis/NodeIndex.h"
#include "analysis/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <algorithm>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
NodeIndex::NodeIndex(TSNode root, const TSLanguage* lang, TraversalBudget* budget)
{
    Build(root, lang, budget);
}

void NodeIndex::PopulatePredefinedSymbols(const TSLanguage* lang)
{
    if (!lang)
    {
        return;
    }
    const uint32_t symCount = ts_language_symbol_count(lang);
    if (m_nodesBySymbol.size() < symCount)
    {
        m_nodesBySymbol.resize(symCount);
    }
    for (const std::string_view name : parser::k_allNodeTypes)
    {
        const TSSymbol sym = ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.length()), true);
        if (sym != 0)
        {
            m_symbolByName[name] = sym;
        }
    }
    for (uint32_t s = 0; s < symCount; ++s)
    {
        const char* name = ts_language_symbol_name(lang, static_cast<TSSymbol>(s));
        if (name)
        {
            m_symbolByName.try_emplace(std::string_view(name), static_cast<TSSymbol>(s));
        }
    }
}

void NodeIndex::IndexNode(TSNode node, TraversalBudget* budget)
{
    if (budget)
    {
        budget->RecordVisit(node);
    }

    m_allNodes.push_back(node);

    if (ts_node_is_error(node) || ts_node_is_missing(node))
    {
        m_errorRanges.push_back(ErrorRange{ts_node_start_byte(node), ts_node_end_byte(node)});
        m_errorNodes.push_back(node);
    }

    const TSSymbol sym = ts_node_symbol(node);
    if (m_symComment != 0 && sym == m_symComment)
    {
        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        m_commentRanges.push_back(
            CommentRange{ts_node_start_byte(node), ts_node_end_byte(node), sp.row, sp.column, ep.row, ep.column});
    }
    if (sym != static_cast<TSSymbol>(-1) && !ts_node_is_error(node) && sym < 4096)
    {
        if (sym < m_nodesBySymbol.size())
        {
            m_nodesBySymbol[sym].push_back(node);
        }
        else
        {
            m_nodesBySymbol.resize(static_cast<size_t>(sym) + 1);
            m_nodesBySymbol[sym].push_back(node);
        }
    }
}

void NodeIndex::TraverseTree(TSNode root, TraversalBudget* budget)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    int depth = 0;
    bool visiting = true;

    while (visiting)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        IndexNode(node, budget);

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

void NodeIndex::Build(TSNode root, const TSLanguage* lang, TraversalBudget* budget)
{
    Clear();
    m_root = root;
    if (ts_node_is_null(root))
    {
        return;
    }

    m_language = lang ? lang : tree_sitter_angelscript();
    PopulatePredefinedSymbols(m_language);
    m_symComment = SymbolForName("comment");
    m_allNodes.reserve(4096);
    TraverseTree(root, budget);
    PostProcessErrorRanges();
    PostProcessCommentRanges();
}

std::span<const TSNode> NodeIndex::Nodes(TSSymbol symbol) const noexcept
{
    if (symbol != static_cast<TSSymbol>(-1) && static_cast<size_t>(symbol) < m_nodesBySymbol.size())
    {
        return m_nodesBySymbol[symbol];
    }
    return {};
}

std::span<const TSNode> NodeIndex::Nodes(std::string_view typeName) const noexcept
{
    if (typeName == "ERROR")
    {
        return m_errorNodes;
    }
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

template <typename RangeType> void MergeSortedRanges(std::vector<RangeType>& ranges)
{
    if (ranges.size() <= 1)
    {
        return;
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const RangeType& a, const RangeType& b) { return a.startByte < b.startByte; });
    std::vector<RangeType> merged;
    merged.reserve(ranges.size());
    merged.push_back(ranges.front());
    for (size_t i = 1; i < ranges.size(); ++i)
    {
        auto& last = merged.back();
        if (ranges[i].startByte <= last.endByte)
        {
            if (ranges[i].endByte > last.endByte)
            {
                last.endByte = ranges[i].endByte;
                if constexpr (requires { last.endLine; })
                {
                    last.endLine = ranges[i].endLine;
                    last.endColumn = ranges[i].endColumn;
                }
            }
        }
        else
        {
            merged.push_back(ranges[i]);
        }
    }
    ranges = std::move(merged);
}

template <typename RangeType> bool IsByteInRanges(std::span<const RangeType> ranges, uint32_t byteOffset) noexcept
{
    if (ranges.empty())
    {
        return false;
    }
    auto it = std::upper_bound(ranges.begin(), ranges.end(), byteOffset,
                               [](uint32_t val, const RangeType& r) { return val < r.startByte; });
    if (it == ranges.begin())
    {
        return false;
    }
    --it;
    return byteOffset >= it->startByte && byteOffset < it->endByte;
}

void NodeIndex::PostProcessErrorRanges()
{
    MergeSortedRanges(m_errorRanges);
}

void NodeIndex::PostProcessCommentRanges()
{
    MergeSortedRanges(m_commentRanges);
}

bool NodeIndex::IsInsideError(uint32_t byteOffset) const noexcept
{
    return IsByteInRanges(std::span<const ErrorRange>(m_errorRanges), byteOffset);
}

bool NodeIndex::IsInsideComment(uint32_t byteOffset) const noexcept
{
    return IsByteInRanges(std::span<const CommentRange>(m_commentRanges), byteOffset);
}

bool NodeIndex::IsInsideCommentLineCol(uint32_t line, uint32_t col) const noexcept
{
    if (m_commentRanges.empty())
    {
        return false;
    }
    auto it = std::upper_bound(m_commentRanges.begin(), m_commentRanges.end(), line,
                               [](uint32_t val, const CommentRange& r) { return val < r.startLine; });
    if (it == m_commentRanges.begin())
    {
        return false;
    }
    --it;
    if (line < it->startLine || line > it->endLine)
    {
        return false;
    }
    if (line == it->startLine && col < it->startColumn)
    {
        return false;
    }
    if (line == it->endLine && col >= it->endColumn)
    {
        return false;
    }
    return true;
}

std::span<const NodeIndex::CommentRange> NodeIndex::CommentRanges() const noexcept
{
    return m_commentRanges;
}

bool NodeIndex::NodeHasSyntaxError(TSNode node) const noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    return ts_node_has_error(node) || IsInsideError(ts_node_start_byte(node));
}

std::span<const NodeIndex::ErrorRange> NodeIndex::ErrorRanges() const noexcept
{
    return m_errorRanges;
}

void NodeIndex::Clear() noexcept
{
    m_root = TSNode{};
    m_language = nullptr;
    m_allNodes.clear();
    m_errorNodes.clear();
    m_nodesBySymbol.clear();
    m_errorRanges.clear();
    m_commentRanges.clear();
    m_symbolByName.clear();
    m_symComment = 0;
}
} // namespace angel_lsp::analysis
