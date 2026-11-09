#include "analysis/index/ClassRuleFacts.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Locates the AST node corresponding to a mixin inclusion in a host class.
 * @param[in] hostNode AST node representing the host class declaration.
 * @param[in] mixinName Full qualified or canonical name of the mixin.
 * @param[in] mixinShortName Unqualified identifier of the mixin.
 * @param[in] sourceCode Document source text buffer.
 * @return AST node representing the mixin inclusion, or a null node if not found.
 */
static TSNode FindMixinInclusionNode(TSNode hostNode, std::string_view mixinName, std::string_view mixinShortName,
                                     std::string_view sourceCode)
{
    if (ts_node_is_null(hostNode))
    {
        return {};
    }

    TSNode found{};
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode baseList = parser::FindChildWithSymbol(hostNode, gs.symBaseClassList);
    if (!ts_node_is_null(baseList))
    {
        parser::ForEachNamedChildNodeUntil(baseList,
                                           [&](TSNode baseChild)
                                           {
                                               const std::string_view bText = parser::NodeText(baseChild, sourceCode);
                                               if (CleanBaseTypeView(bText) == mixinName || bText == mixinShortName ||
                                                   utils::EndsWithScopedMember(bText, mixinShortName))
                                               {
                                                   found = baseChild;
                                                   return false;
                                               }
                                               return true;
                                           });
        if (!ts_node_is_null(found))
        {
            return found;
        }
    }

    const TSNode bodyNode = parser::GetChildByField(hostNode, gs.fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        parser::ForEachChildNodeUntil(bodyNode,
                                      [&](TSNode mNode)
                                      {
                                          const std::string_view mText = parser::NodeText(mNode, sourceCode);
                                          if (!mixinShortName.empty() &&
                                              mText.find(mixinShortName) != std::string_view::npos)
                                          {
                                              found = mNode;
                                              return false;
                                          }
                                          return true;
                                      });
    }
    return found;
}

/**
 * @brief Extracts source spans for all base class or interface entries in an inheritance list.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] startLine 0-based start line of the enclosing class or interface symbol.
 * @param[in] endLine 0-based end line of the enclosing class or interface symbol.
 * @return One span per named child of the first base_class_list node starting within
 *         [startLine, endLine]; empty when no base_class_list exists in range.
 */
std::vector<IndexedSourceSpan> BuildBaseClassSpans(const NodeIndex& index, uint32_t startLine, uint32_t endLine)
{
    const auto& gs = parser::GetGrammarSymbols();
    for (const TSNode baseList : index.Nodes(gs.symBaseClassList))
    {
        const TSPoint pt = ts_node_start_point(baseList);
        if (pt.row >= startLine && pt.row <= endLine)
        {
            std::vector<IndexedSourceSpan> spans;
            parser::ForEachNamedChildNode(baseList,
                                          [&spans](TSNode child) { spans.push_back(MakeIndexedSourceSpan(child)); });
            return spans;
        }
    }
    return {};
}

/**
 * @brief Locates the source span of a mixin inclusion statement within a host class declaration.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @param[in] query Mixin inclusion query specifying host position and mixin names.
 * @return Source span of the mixin inclusion entry in the enclosing host class;
 *         std::nullopt when the index has no tree, no host class, or no inclusion.
 */
std::optional<IndexedSourceSpan> FindMixinInclusionSpan(const NodeIndex& index, std::string_view source,
                                                        const MixinInclusionQuery& query)
{
    if (source.empty())
    {
        return std::nullopt;
    }

    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const TSPoint hostPt{query.hostPos.line, query.hostPos.character};
    TSNode hostNode = ts_node_descendant_for_point_range(rootNode, hostPt, hostPt);
    if (ts_node_is_null(hostNode))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    hostNode = parser::FindAncestorWithSymbol(hostNode, gs.symClassDeclaration);
    if (ts_node_is_null(hostNode))
    {
        return std::nullopt;
    }

    const TSNode incNode = FindMixinInclusionNode(hostNode, query.mixinName, query.mixinShortName, source);
    if (ts_node_is_null(incNode))
    {
        return std::nullopt;
    }

    return MakeIndexedSourceSpan(incNode);
}

} // namespace angel_lsp::analysis
