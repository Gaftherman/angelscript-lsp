#include "analysis/index/NodeIndex.h"

#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
NodeIndex::NodeIndex(TSNode root, const TSLanguage* lang, TraversalBudget* budget)
{
    Build(root, lang, budget);
}

const ankerl::unordered_dense::map<std::string_view, TSSymbol>& NodeIndex::GetSymbolNameMap() noexcept
{
    static const auto s_map = []()
    {
        ankerl::unordered_dense::map<std::string_view, TSSymbol> map;
        const TSLanguage* lang = tree_sitter_angelscript();
        const uint32_t symCount = ts_language_symbol_count(lang);
        for (const std::string_view name : parser::k_allNodeTypes)
        {
            const TSSymbol sym =
                ts_language_symbol_for_name(lang, name.data(), static_cast<uint32_t>(name.length()), true);
            if (sym != 0)
            {
                map[name] = sym;
            }
        }
        for (uint32_t s = 0; s < symCount; ++s)
        {
            const char* name = ts_language_symbol_name(lang, static_cast<TSSymbol>(s));
            if (name)
            {
                map.try_emplace(std::string_view(name), static_cast<TSSymbol>(s));
            }
        }
        return map;
    }();
    return s_map;
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
}

void NodeIndex::IndexNode(TSNode node, TraversalBudget* budget, uint32_t parentSpanIdx)
{
    if (budget)
    {
        budget->RecordVisit(node);
    }

    ++m_nodeCount;

    if (ts_node_is_error(node) || ts_node_is_missing(node))
    {
        m_errorRanges.push_back(ErrorRange{ts_node_start_byte(node), ts_node_end_byte(node)});
        m_errorNodes.push_back(node);
    }

    const TSSymbol sym = ts_node_symbol(node);
    const auto& gs = parser::GetGrammarSymbols();
    if (m_symComment != 0 && sym == m_symComment)
    {
        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        m_commentRanges.push_back(
            CommentRange{ts_node_start_byte(node), ts_node_end_byte(node), sp.row, sp.column, ep.row, ep.column});
    }

    const bool isNamed = ts_node_is_named(node);
    const bool isQueriedAnonymous = (sym == gs.tokShared || sym == gs.tokMixin || sym == m_symComment);

    if ((isNamed || isQueriedAnonymous) && sym != static_cast<TSSymbol>(-1) && !ts_node_is_error(node) && sym < 4096)
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

        RecordContainerSpan(node, sym, parentSpanIdx);
    }
}

void NodeIndex::RecordContainerSpan(TSNode node, TSSymbol sym, uint32_t parentSpanIdx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration || sym == gs.symInterfaceDeclaration ||
        sym == gs.symNamespaceDeclaration || sym == gs.symLambdaExpression)
    {
        IndexedContainerSpan span;
        span.startByte = ts_node_start_byte(node);
        span.endByte = ts_node_end_byte(node);
        span.parentSpanIndex = parentSpanIdx;
        span.symbol = sym;
        if (sym != gs.symLambdaExpression)
        {
            TSNode nameNode = parser::GetChildByField(node, gs.fieldName);
            if (!ts_node_is_null(nameNode))
            {
                span.nameStartByte = ts_node_start_byte(nameNode);
                span.nameEndByte = ts_node_end_byte(nameNode);
            }
        }
        span.isMixin =
            ((sym == gs.symMixinDeclaration) || !ts_node_is_null(parser::FindChildWithSymbol(node, gs.tokMixin))) ? 1
                                                                                                                  : 0;
        m_containerSpans.push_back(span);
    }
}

static bool AdvanceTreeCursor(TSTreeCursor* cursor) noexcept
{
    if (ts_tree_cursor_goto_first_child(cursor))
    {
        return true;
    }
    if (ts_tree_cursor_goto_next_sibling(cursor))
    {
        return true;
    }
    while (ts_tree_cursor_goto_parent(cursor))
    {
        if (ts_tree_cursor_goto_next_sibling(cursor))
        {
            return true;
        }
    }
    return false;
}

static void MaintainActiveContainers(std::vector<uint32_t>& activeContainers,
                                     std::span<const IndexedContainerSpan> spans, uint32_t startByte)
{
    while (!activeContainers.empty() && startByte >= spans[activeContainers.back()].endByte)
    {
        activeContainers.pop_back();
    }
}

/**
 * @brief Traverses the AST tree iteratively using a flat cursor and indexes nodes of interest.
 * @param[in] root Root AST node to traverse.
 * @param[in] budget Traversal budget guard to prevent runaway recursion or unbounded visits.
 */
void NodeIndex::TraverseTree(TSNode root, TraversalBudget* budget)
{
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    std::vector<uint32_t> activeContainers;

    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        MaintainActiveContainers(activeContainers, m_containerSpans, ts_node_start_byte(node));

        const uint32_t parentSpanIdx = activeContainers.empty() ? UINT32_MAX : activeContainers.back();
        const size_t containerCount = m_containerSpans.size();
        IndexNode(node, budget, parentSpanIdx);
        if (m_containerSpans.size() > containerCount)
        {
            activeContainers.push_back(static_cast<uint32_t>(containerCount));
        }

        if (!AdvanceTreeCursor(&cursor))
        {
            break;
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
    TraverseTree(root, budget);
    PostProcessErrorRanges();
    PostProcessCommentRanges();
}

std::span<const TSNode> NodeIndex::Nodes(TSSymbol symbol) const noexcept
{
    if (symbol == parser::GetGrammarSymbols().symError || symbol == static_cast<TSSymbol>(-1))
    {
        return m_errorNodes;
    }
    if (static_cast<size_t>(symbol) < m_nodesBySymbol.size())
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
    const auto& map = GetSymbolNameMap();
    const auto it = map.find(typeName);
    if (it != map.end())
    {
        return Nodes(it->second);
    }
    return {};
}

TSSymbol NodeIndex::SymbolForName(std::string_view typeName) const noexcept
{
    const auto& map = GetSymbolNameMap();
    const auto it = map.find(typeName);
    if (it != map.end())
    {
        return it->second;
    }
    return 0;
}

TSNode NodeIndex::Root() const noexcept
{
    return m_root;
}

size_t NodeIndex::NodeCount() const noexcept
{
    return m_nodeCount;
}

bool NodeIndex::Empty() const noexcept
{
    return m_nodeCount == 0;
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
    m_nodeCount = 0;
    m_errorNodes.clear();
    m_nodesBySymbol.clear();
    m_errorRanges.clear();
    m_commentRanges.clear();
    m_containerSpans.clear();
    m_symComment = 0;
}

TSNode NodeIndex::FindEnclosingNode(uint32_t byteOffset, TSSymbol targetSym) const noexcept
{
    TSNode bestNode{};
    uint32_t bestSpan = UINT32_MAX;
    for (TSNode node : Nodes(targetSym))
    {
        const uint32_t start = ts_node_start_byte(node);
        if (start > byteOffset)
        {
            break;
        }
        const uint32_t end = ts_node_end_byte(node);
        if (byteOffset < end)
        {
            const uint32_t span = end - start;
            if (span < bestSpan)
            {
                bestSpan = span;
                bestNode = node;
            }
        }
    }
    return bestNode;
}

TSNode NodeIndex::FindEnclosingNodeAny(uint32_t byteOffset, std::span<const TSSymbol> targetSyms) const noexcept
{
    TSNode bestNode{};
    uint32_t bestSpan = UINT32_MAX;
    for (TSSymbol sym : targetSyms)
    {
        for (TSNode node : Nodes(sym))
        {
            const uint32_t start = ts_node_start_byte(node);
            if (start > byteOffset)
            {
                break;
            }
            const uint32_t end = ts_node_end_byte(node);
            if (byteOffset < end)
            {
                const uint32_t span = end - start;
                if (span < bestSpan)
                {
                    bestSpan = span;
                    bestNode = node;
                }
            }
        }
    }
    return bestNode;
}

TSNode NodeIndex::FindEnclosingClassOrMixin(uint32_t byteOffset) const noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targets[] = {gs.symClassDeclaration, gs.symInterfaceDeclaration, gs.symMixinDeclaration};
    return FindEnclosingNodeAny(byteOffset, targets);
}

const IndexedContainerSpan* NodeIndex::FindInnermostClassSpan(uint32_t byteOffset) const noexcept
{
    if (m_containerSpans.empty())
    {
        return nullptr;
    }
    const auto& gs = parser::GetGrammarSymbols();
    auto it = std::upper_bound(m_containerSpans.begin(), m_containerSpans.end(), byteOffset,
                               [](uint32_t val, const IndexedContainerSpan& s) { return val < s.startByte; });
    while (it != m_containerSpans.begin())
    {
        --it;
        if (byteOffset < it->endByte)
        {
            if (it->symbol == gs.symLambdaExpression)
            {
                return nullptr;
            }
            if (it->symbol == gs.symClassDeclaration || it->symbol == gs.symMixinDeclaration)
            {
                return &(*it);
            }
            return nullptr;
        }
    }
    return nullptr;
}

TSNode NodeIndex::FindEnclosingCallable(uint32_t byteOffset) const noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol targets[] = {gs.symFuncDeclaration, gs.symLambdaExpression};
    return FindEnclosingNodeAny(byteOffset, targets);
}

bool NodeIndex::IsInsideLambdaClosure(uint32_t byteOffset) const noexcept
{
    const TSNode callable = FindEnclosingCallable(byteOffset);
    return !ts_node_is_null(callable) && ts_node_symbol(callable) == parser::GetGrammarSymbols().symLambdaExpression;
}

/**
 * @brief Classify index container kind.
 * @param[in] sym Tree-sitter grammar symbol.
 * @return Found item if present; std::nullopt otherwise.
 */

std::optional<ContainerKind> ClassifyContainerKind(TSSymbol sym) noexcept
{
    const auto& gs = parser::GetGrammarSymbols();
    if (sym == gs.symClassDeclaration || sym == gs.symMixinDeclaration)
    {
        return ContainerKind::Class;
    }
    if (sym == gs.symInterfaceDeclaration)
    {
        return ContainerKind::Interface;
    }
    if (sym == gs.symNamespaceDeclaration)
    {
        return ContainerKind::Namespace;
    }
    return std::nullopt;
}

/**
 * @brief Extracts container name and byte range from a name AST node and appends a ContainerInfo entry.
 * @param[in] nameNode AST node of the container name identifier.
 * @param[in] kind Categorical ContainerKind (e.g. Class, Interface, Namespace).
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] rawContainers Output vector accumulating parsed container definitions.
 */
void AppendRawContainers(TSNode nameNode, ContainerKind kind, std::string_view sourceCode,
                         std::vector<ContainerInfo>& rawContainers)
{
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    const uint32_t start = ts_node_start_byte(nameNode);
    const uint32_t end = ts_node_end_byte(nameNode);
    if (start >= end || end > sourceCode.size())
    {
        return;
    }

    std::string text(sourceCode.substr(start, end - start));
    if (kind == ContainerKind::Namespace && utils::ContainsScopeSeparator(text))
    {
        auto segs = SplitScopeSegments(text);
        for (auto it = segs.rbegin(); it != segs.rend(); ++it)
        {
            if (!it->empty())
            {
                rawContainers.push_back({*it, "", kind});
            }
        }
    }
    else
    {
        rawContainers.push_back({std::move(text), "", kind});
    }
}

/**
 * @brief Compute index container qualified names.
 * @param[in,out] containers Vector of ContainerInfo structs whose qualified names are computed.
 */
void ComputeContainerQualifiedNames(std::vector<ContainerInfo>& containers)
{
    for (size_t i = 0; i < containers.size(); ++i)
    {
        std::string qName;
        for (size_t j = containers.size(); j > i; --j)
        {
            if (!qName.empty())
            {
                qName += "::";
            }
            qName += containers[j - 1].name;
        }
        containers[i].qualifiedName = std::move(qName);
    }
}

static uint32_t FindInnermostContainerSpanIndex(std::span<const IndexedContainerSpan> spans,
                                                uint32_t byteOffset) noexcept
{
    auto it = std::upper_bound(spans.begin(), spans.end(), byteOffset,
                               [](uint32_t val, const IndexedContainerSpan& s) { return val < s.startByte; });

    while (it != spans.begin())
    {
        --it;
        if (byteOffset < it->endByte)
        {
            return static_cast<uint32_t>(std::distance(spans.begin(), it));
        }
    }
    return UINT32_MAX;
}

static void AppendContainerFromSpan(const IndexedContainerSpan& span, ContainerKind kind, std::string_view sourceCode,
                                    std::vector<ContainerInfo>& rawContainers)
{
    if (span.nameStartByte >= span.nameEndByte || span.nameEndByte > sourceCode.size())
    {
        return;
    }
    std::string text(sourceCode.substr(span.nameStartByte, span.nameEndByte - span.nameStartByte));
    if (kind == ContainerKind::Namespace && utils::ContainsScopeSeparator(text))
    {
        auto segs = SplitScopeSegments(text);
        for (auto rit = segs.rbegin(); rit != segs.rend(); ++rit)
        {
            if (!rit->empty())
            {
                rawContainers.push_back({*rit, "", kind});
            }
        }
    }
    else
    {
        rawContainers.push_back({std::move(text), "", kind});
    }
}

std::vector<ContainerInfo> NodeIndex::CollectEnclosingContainers(uint32_t byteOffset, std::string_view sourceCode) const
{
    std::vector<ContainerInfo> rawContainers;
    if (m_containerSpans.empty())
    {
        return rawContainers;
    }

    const uint32_t innermostIdx = FindInnermostContainerSpanIndex(m_containerSpans, byteOffset);
    if (innermostIdx == UINT32_MAX)
    {
        return rawContainers;
    }

    const auto& gs = parser::GetGrammarSymbols();
    uint32_t curr = innermostIdx;
    bool crossedLambda = false;

    while (curr != UINT32_MAX)
    {
        const auto& span = m_containerSpans[curr];
        if (span.symbol == gs.symLambdaExpression)
        {
            crossedLambda = true;
        }
        else if (const auto kind = ClassifyContainerKind(span.symbol))
        {
            if (!(crossedLambda && (*kind == ContainerKind::Class || *kind == ContainerKind::Interface)))
            {
                AppendContainerFromSpan(span, *kind, sourceCode, rawContainers);
            }
        }
        curr = span.parentSpanIndex;
    }

    ComputeContainerQualifiedNames(rawContainers);
    return rawContainers;
}
} // namespace angel_lsp::analysis
