#include "analysis/index/SemanticTokenFacts.h"

#include "analysis/index/SemanticTokenFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/QueryRegistry.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Builds class spans for class declarations in the AST, optionally restricted to a byte range.
 * @param[in] index Syntax node index.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] byteRange Optional byte range to filter overlapping class declarations.
 * @param[out] classSpans Output vector of IndexedClassSpan items.
 */
static void BuildClassSpans(const NodeIndex& index, std::string_view sourceCode,
                            std::optional<IndexedByteRange> byteRange, std::vector<IndexedClassSpan>& classSpans)
{
    const auto& syms = parser::GetGrammarSymbols();
    for (TSNode classDecl : index.Nodes(syms.symClassDeclaration))
    {
        if (byteRange.has_value())
        {
            const uint32_t declStart = ts_node_start_byte(classDecl);
            const uint32_t declEnd = ts_node_end_byte(classDecl);
            if (declEnd <= byteRange->startByte || declStart >= byteRange->endByte)
            {
                continue;
            }
        }
        TSNode classNameNode = parser::GetChildByField(classDecl, syms.fieldName);
        if (!ts_node_is_null(classNameNode))
        {
            uint32_t cStart = ts_node_start_byte(classNameNode);
            uint32_t cEnd = ts_node_end_byte(classNameNode);
            if (cStart < cEnd && cEnd <= sourceCode.size())
            {
                std::string_view bareName(sourceCode.data() + cStart, cEnd - cStart);
                const auto containers = index.CollectLookupContainers(
                    ts_node_start_byte(classDecl), ts_node_end_byte(classDecl), syms.symClassDeclaration, sourceCode);
                const std::string qName = (!containers.empty() && !containers.front().qualifiedName.empty())
                                              ? utils::JoinScopedName(containers.front().qualifiedName, bareName)
                                              : std::string(bareName);
                classSpans.push_back(
                    IndexedClassSpan{ts_node_start_byte(classDecl), ts_node_end_byte(classDecl), std::move(qName), -1});
            }
        }
    }

    std::sort(classSpans.begin(), classSpans.end(),
              [](const IndexedClassSpan& a, const IndexedClassSpan& b)
              {
                  if (a.startByte != b.startByte)
                  {
                      return a.startByte < b.startByte;
                  }
                  return a.endByte > b.endByte;
              });

    std::vector<int> spanStack;
    for (size_t i = 0; i < classSpans.size(); ++i)
    {
        while (!spanStack.empty() && classSpans[spanStack.back()].endByte <= classSpans[i].startByte)
        {
            spanStack.pop_back();
        }
        if (!spanStack.empty() && classSpans[spanStack.back()].endByte >= classSpans[i].endByte)
        {
            classSpans[i].parentIndex = spanStack.back();
        }
        spanStack.push_back(static_cast<int>(i));
    }
}

std::string_view SemanticTokenFacts::FindEnclosingClass(uint32_t byteOffset) const
{
    if (classSpans.empty())
    {
        return {};
    }

    auto it = std::upper_bound(classSpans.begin(), classSpans.end(), byteOffset,
                               [](uint32_t offset, const IndexedClassSpan& span) { return offset < span.startByte; });

    if (it == classSpans.begin())
    {
        return {};
    }

    const int cand = static_cast<int>(std::distance(classSpans.begin(), it)) - 1;
    for (int cur = cand; cur != -1; cur = classSpans[cur].parentIndex)
    {
        if (byteOffset >= classSpans[cur].startByte && byteOffset < classSpans[cur].endByte)
        {
            return classSpans[cur].qualifiedName;
        }
    }
    return {};
}

/**
 * @brief Flags a capture that is the last segment of a scoped name inside a base-class list.
 * @param[in] node Highlight capture node.
 * @param[in] syms Grammar symbols table.
 * @param[in,out] cap Capture record receiving the flag and the qualifier byte range.
 */
static void MarkBaseListScopedLeaf(TSNode node, const parser::GrammarSymbols& syms, IndexedHighlightCapture& cap)
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != syms.symScopedIdentifier)
    {
        return;
    }
    const uint32_t count = ts_node_named_child_count(parent);
    if (count <= 1 || ts_node_start_byte(ts_node_named_child(parent, count - 1)) != cap.startByte)
    {
        return;
    }
    TSNode grandParent = parser::GetParent(parent);
    if (ts_node_is_null(grandParent) || ts_node_symbol(grandParent) != syms.symBaseClassList)
    {
        return;
    }
    cap.isBaseListScopedLeaf = true;
    TSNode firstChild = ts_node_named_child(parent, 0);
    if (!ts_node_is_null(firstChild))
    {
        cap.qualifierStartByte = ts_node_start_byte(firstChild);
        cap.qualifierEndByte = ts_node_end_byte(firstChild);
    }
}

/**
 * @brief Records the scope and member segments of a `Scope::Type` type node for enum upgrades.
 * @param[in] typeNode Type node whose first named children are a scope and a datatype.
 * @param[in] sourceCode Document source text.
 * @param[in] syms Grammar symbols table.
 * @param[in,out] scopedTypes Destination receiving one record when both segments are non-empty.
 */
static void AppendScopedType(TSNode typeNode, std::string_view sourceCode, const parser::GrammarSymbols& syms,
                             std::vector<IndexedScopedType>& scopedTypes)
{
    if (ts_node_named_child_count(typeNode) < 2)
    {
        return;
    }
    TSNode firstChild = ts_node_named_child(typeNode, 0);
    TSNode secondChild = ts_node_named_child(typeNode, 1);
    if (ts_node_symbol(firstChild) != syms.symScope || ts_node_symbol(secondChild) != syms.symDatatype)
    {
        return;
    }

    std::string_view scopeText =
        utils::StripGlobalPrefix(utils::StripTrailingScopeSeparator(parser::NodeText(firstChild, sourceCode)));
    while (scopeText.ends_with(':'))
    {
        scopeText.remove_suffix(1);
    }
    const std::string_view memberText = parser::NodeText(secondChild, sourceCode);
    if (scopeText.empty() || memberText.empty())
    {
        return;
    }

    TSNode scopeId = ts_node_named_child(firstChild, 0);
    if (!ts_node_is_null(scopeId) && ts_node_named_child_count(scopeId) > 0)
    {
        scopeId = ts_node_named_child(scopeId, ts_node_named_child_count(scopeId) - 1);
    }
    TSNode memberId = ts_node_named_child(secondChild, 0);

    IndexedScopedType st;
    st.scopeTextStartByte = static_cast<uint32_t>(scopeText.data() - sourceCode.data());
    st.scopeTextEndByte = st.scopeTextStartByte + static_cast<uint32_t>(scopeText.size());
    st.memberTextStartByte = static_cast<uint32_t>(memberText.data() - sourceCode.data());
    st.memberTextEndByte = st.memberTextStartByte + static_cast<uint32_t>(memberText.size());
    st.scopeUpgradeByte = ts_node_is_null(scopeId) ? ts_node_start_byte(firstChild) : ts_node_start_byte(scopeId);
    st.memberUpgradeByte = ts_node_is_null(memberId) ? ts_node_start_byte(secondChild) : ts_node_start_byte(memberId);
    scopedTypes.push_back(st);
}

/**
 * @brief Computes half-open byte bounds in sourceCode for a 0-based line/column SourceRange.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] range Target line/column range.
 * @return Clamped IndexedByteRange covering the requested range.
 */
static IndexedByteRange ComputeRangeByteBounds(std::string_view sourceCode, const SourceRange& range) noexcept
{
    uint32_t startByte = static_cast<uint32_t>(sourceCode.size());
    uint32_t endByte = static_cast<uint32_t>(sourceCode.size());
    uint32_t line = 0;
    size_t pos = 0;
    bool foundStart = false;

    while (pos <= sourceCode.size())
    {
        const size_t nl = sourceCode.find('\n', pos);
        const size_t lineEnd = (nl == std::string_view::npos) ? sourceCode.size() : nl;
        if (!foundStart && line == range.startLine)
        {
            startByte = static_cast<uint32_t>(std::min(pos + range.startCharacter, lineEnd));
            foundStart = true;
        }
        if (line == range.endLine)
        {
            endByte = static_cast<uint32_t>(std::min(pos + range.endCharacter, lineEnd));
            break;
        }
        if (nl == std::string_view::npos)
        {
            break;
        }
        pos = nl + 1;
        ++line;
    }

    if (endByte < startByte)
    {
        endByte = startByte;
    }
    return IndexedByteRange{startByte, endByte};
}

/**
 * @brief Returns the contiguous slice of ordered nodes that may overlap a byte range.
 * @param[in] nodes Ordered span of AST nodes sorted by start byte.
 * @param[in] byteRange Target byte interval.
 * @return Subspan of nodes starting at or before byteRange.startByte and before byteRange.endByte.
 */
static std::span<const TSNode> FindOverlappingNodes(std::span<const TSNode> nodes, IndexedByteRange byteRange) noexcept
{
    if (nodes.empty() || byteRange.startByte >= byteRange.endByte)
    {
        return {};
    }
    auto endIt = std::lower_bound(nodes.begin(), nodes.end(), byteRange.endByte,
                                  [](TSNode n, uint32_t b) { return ts_node_start_byte(n) < b; });
    auto startIt = std::lower_bound(nodes.begin(), endIt, byteRange.startByte,
                                    [](TSNode n, uint32_t b) { return ts_node_start_byte(n) < b; });
    while (startIt != nodes.begin())
    {
        auto prev = std::prev(startIt);
        if (ts_node_end_byte(*prev) > byteRange.startByte)
        {
            startIt = prev;
        }
        else if (prev != nodes.begin() && ts_node_start_byte(*std::prev(prev)) == ts_node_start_byte(*prev))
        {
            startIt = prev;
        }
        else
        {
            break;
        }
    }
    return nodes.subspan(static_cast<size_t>(startIt - nodes.begin()), static_cast<size_t>(endIt - startIt));
}

/**
 * @brief Shared implementation for full and ranged semantic token fact extraction.
 * @param[in] index Pre-indexed syntax tree facts.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] range Optional line/column range to restrict extraction.
 * @return Populated SemanticTokenFacts collection.
 */
static SemanticTokenFacts CollectSemanticTokenFactsImpl(const NodeIndex& index, std::string_view sourceCode,
                                                        const std::optional<SourceRange>& range)
{
    SemanticTokenFacts facts;
    if (sourceCode.empty())
    {
        return facts;
    }
    if (range.has_value() && (range->endLine < range->startLine ||
                              (range->endLine == range->startLine && range->endCharacter < range->startCharacter) ||
                              (range->endLine == 0 && range->endCharacter == 0)))
    {
        return facts;
    }

    const auto& highlightsData = GetHighlightsQueryData();
    TSNode root = index.Root();
    if (!highlightsData.query || ts_node_is_null(root))
    {
        return facts;
    }

    const auto& syms = parser::GetGrammarSymbols();
    const std::optional<IndexedByteRange> byteRange =
        range.has_value() ? std::optional<IndexedByteRange>(ComputeRangeByteBounds(sourceCode, *range)) : std::nullopt;

    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (range.has_value())
    {
        ts_query_cursor_set_point_range(cursor, TSPoint{range->startLine, range->startCharacter},
                                        TSPoint{range->endLine, range->endCharacter});
    }
    else
    {
        ts_query_cursor_set_point_range(cursor, TSPoint{0, 0}, TSPoint{0, 0});
    }
    ts_query_cursor_exec(cursor, highlightsData.query, root);

    TSQueryMatch match;
    uint32_t captureIndex = 0;
    while (ts_query_cursor_next_capture(cursor, &match, &captureIndex))
    {
        TSNode node = match.captures[captureIndex].node;
        uint32_t captureId = match.captures[captureIndex].index;
        if (captureId >= highlightsData.rules.size())
        {
            continue;
        }

        const auto& rule = highlightsData.rules[captureId];
        if (!rule.valid)
        {
            continue;
        }

        uint32_t tokenType = rule.tokenType;
        uint32_t tokenMod = rule.tokenMod;
        int priority = rule.priority;

        if (rule.isOperatorOrPunctuation)
        {
            auto opRes = ClassifyOperatorOrPunctuation(node, sourceCode, syms);
            if (!opRes.has_value())
            {
                continue;
            }
            tokenType = opRes->first;
            priority = opRes->second;
        }

        IndexedHighlightCapture cap;
        cap.startByte = ts_node_start_byte(node);
        cap.endByte = ts_node_end_byte(node);
        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        cap.startLine = sp.row;
        cap.startColumn = sp.column;
        cap.endLine = ep.row;
        cap.endColumn = ep.column;
        cap.tokenType = tokenType;
        cap.tokenModifiers = tokenMod;
        cap.priority = priority;
        // Only operator tokens are ever retyped as template punctuation, so the two parent hops
        // are skipped for every other capture.
        cap.isTemplatePunctuationNode = tokenType == Type_Operator && IsTemplatePunctuationNode(node, syms);
        MarkBaseListScopedLeaf(node, syms, cap);
        facts.captures.push_back(cap);
    }
    ts_query_cursor_set_point_range(cursor, TSPoint{0, 0}, TSPoint{0, 0});

    const std::span<const TSNode> scopedNodes =
        byteRange.has_value() ? FindOverlappingNodes(index.Nodes(syms.symScopedIdentifier), *byteRange)
                              : index.Nodes(syms.symScopedIdentifier);
    for (TSNode scopedNode : scopedNodes)
    {
        if (byteRange.has_value() && ts_node_end_byte(scopedNode) <= byteRange->startByte)
        {
            continue;
        }
        const uint32_t namedCount = ts_node_named_child_count(scopedNode);
        if (namedCount >= 2)
        {
            TSNode left = ts_node_named_child(scopedNode, 0);
            TSNode prev = ts_node_named_child(scopedNode, namedCount - 2);
            TSNode right = ts_node_named_child(scopedNode, namedCount - 1);
            IndexedScopedName item;
            item.leftStartByte = ts_node_start_byte(left);
            item.leftEndByte = ts_node_end_byte(left);
            item.prevStartByte = ts_node_start_byte(prev);
            item.prevEndByte = ts_node_end_byte(prev);
            item.rightStartByte = ts_node_start_byte(right);
            item.rightEndByte = ts_node_end_byte(right);
            item.nodeStartByte = ts_node_start_byte(scopedNode);
            item.nodeEndByte = ts_node_end_byte(scopedNode);
            facts.scopedNames.push_back(item);
        }
    }

    const std::span<const TSNode> scopeNodes = byteRange.has_value()
                                                   ? FindOverlappingNodes(index.Nodes(syms.symScope), *byteRange)
                                                   : index.Nodes(syms.symScope);
    for (TSNode scopeNode : scopeNodes)
    {
        if (byteRange.has_value() && ts_node_end_byte(scopeNode) <= byteRange->startByte)
        {
            continue;
        }
        TSNode parent = parser::GetParent(scopeNode);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == syms.symType)
        {
            AppendScopedType(parent, sourceCode, syms, facts.scopedTypes);
        }
    }

    BuildClassSpans(index, sourceCode, byteRange, facts.classSpans);

    return facts;
}

SemanticTokenFacts CollectSemanticTokenFacts(const NodeIndex& index, std::string_view sourceCode)
{
    return CollectSemanticTokenFactsImpl(index, sourceCode, std::nullopt);
}

SemanticTokenFacts CollectSemanticTokenFacts(const NodeIndex& index, std::string_view sourceCode,
                                             const SourceRange& range)
{
    return CollectSemanticTokenFactsImpl(index, sourceCode, range);
}

} // namespace angel_lsp::analysis
