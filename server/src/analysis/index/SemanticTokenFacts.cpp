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
 * @brief Builds class spans for all class declarations in the AST.
 * @param[in] index Syntax node index.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] syms Grammar symbols table.
 * @param[out] classSpans Output vector of IndexedClassSpan items.
 */
static void BuildClassSpans(const NodeIndex& index, std::string_view sourceCode, const parser::GrammarSymbols& syms,
                            std::vector<IndexedClassSpan>& classSpans)
{
    for (TSNode classDecl : index.Nodes(syms.symClassDeclaration))
    {
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

SemanticTokenFacts CollectSemanticTokenFacts(const NodeIndex& index, std::string_view sourceCode)
{
    SemanticTokenFacts facts;
    if (sourceCode.empty())
    {
        return facts;
    }

    const auto& highlightsData = GetHighlightsQueryData();
    if (!highlightsData.query)
    {
        return facts;
    }

    TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return facts;
    }

    const auto& syms = parser::GetGrammarSymbols();

    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
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
        cap.isTemplatePunctuationNode = IsTemplatePunctuationNode(node, syms);
        MarkBaseListScopedLeaf(node, syms, cap);
        facts.captures.push_back(cap);
    }

    for (TSNode scopedNode : index.Nodes(syms.symScopedIdentifier))
    {
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

    for (TSNode scopeNode : index.Nodes(syms.symScope))
    {
        TSNode parent = parser::GetParent(scopeNode);
        if (!ts_node_is_null(parent) && ts_node_symbol(parent) == syms.symType)
        {
            AppendScopedType(parent, sourceCode, syms, facts.scopedTypes);
        }
    }

    BuildClassSpans(index, sourceCode, syms, facts.classSpans);

    return facts;
}

} // namespace angel_lsp::analysis
