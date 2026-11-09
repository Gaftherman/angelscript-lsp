#include "analysis/index/ClassRuleFacts.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Iterates over indexed nodes whose byte bounds lie strictly within [startByte, endByte].
 * @param[in] nodes Slice of indexed AST nodes ordered by start byte.
 * @param[in] startByte Half-open starting byte offset.
 * @param[in] endByte Half-open ending byte offset.
 * @param[in] action Functor invoked for each matching node within byte bounds.
 */
template <typename Action>
static void ForEachBoundedNode(std::span<const TSNode> nodes, uint32_t startByte, uint32_t endByte, Action&& action)
{
    auto it = std::lower_bound(nodes.begin(), nodes.end(), startByte,
                               [](const TSNode& n, uint32_t b) { return ts_node_start_byte(n) < b; });
    for (; it != nodes.end() && ts_node_start_byte(*it) < endByte; ++it)
    {
        if (ts_node_end_byte(*it) <= endByte)
        {
            action(*it);
        }
    }
}

/**
 * @brief Locates the AST node declaring the mixin or class at the given source position.
 * @param[in] root AST root node of the document.
 * @param[in] mixinPos 0-based source coordinate of the mixin declaration symbol.
 * @return AST node representing the declaration, or a null node if not found.
 */
static TSNode FindMixinAstNode(TSNode root, IndexedSourcePoint mixinPos)
{
    if (ts_node_is_null(root))
    {
        return TSNode{};
    }
    const TSPoint pt = {mixinPos.line, mixinPos.character};
    const TSNode mixinNode = ts_node_descendant_for_point_range(root, pt, pt);
    if (ts_node_is_null(mixinNode))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 2> targets = {gs.symMixinDeclaration, gs.symClassDeclaration};
    return parser::FindAncestorWithAnySymbol(mixinNode, targets);
}

/**
 * @brief Collects byte ranges of parameter and local variable names within a mixin function.
 * @param[in] funcNode AST node of the function declaration.
 * @param[in] funcBody AST node of the function body.
 * @param[in] index Node index of the current syntax revision.
 * @param[out] localNames Destination vector for local identifier byte ranges.
 */
static void CollectLocalNames(TSNode funcNode, TSNode funcBody, const NodeIndex& index,
                              std::vector<IndexedByteRange>& localNames)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode paramsNode = parser::GetChildByField(funcNode, gs.fieldParameters);
    if (!ts_node_is_null(paramsNode))
    {
        parser::ForEachNamedChildNode(paramsNode,
                                      [&](TSNode paramChild)
                                      {
                                          const TSNode pName = parser::GetChildByField(paramChild, gs.fieldName);
                                          if (!ts_node_is_null(pName))
                                          {
                                              localNames.push_back(
                                                  IndexedByteRange{ts_node_start_byte(pName), ts_node_end_byte(pName)});
                                          }
                                      });
    }
    const uint32_t startByte = ts_node_start_byte(funcBody);
    const uint32_t endByte = ts_node_end_byte(funcBody);
    const auto recordName = [&](TSNode n)
    {
        const TSNode vdName = parser::GetChildByField(n, gs.fieldName);
        if (!ts_node_is_null(vdName))
        {
            localNames.push_back(IndexedByteRange{ts_node_start_byte(vdName), ts_node_end_byte(vdName)});
        }
    };
    ForEachBoundedNode(index.Nodes(gs.symVariableDeclarator), startByte, endByte, recordName);
    ForEachBoundedNode(index.Nodes(gs.symForeachVariable), startByte, endByte, recordName);
}

/**
 * @brief Appends this-member and call-member references occurring in a function body.
 * @param[in] funcBody AST node of the function body.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @param[out] refs Destination collection for mixin member references.
 */
static void AppendMemberRefs(TSNode funcBody, const NodeIndex& index, std::string_view source,
                             std::vector<IndexedMixinMemberRef>& refs)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t startByte = ts_node_start_byte(funcBody);
    const uint32_t endByte = ts_node_end_byte(funcBody);
    ForEachBoundedNode(
        index.Nodes(gs.symMemberExpression), startByte, endByte,
        [&](TSNode n)
        {
            const TSNode objNode = parser::GetChildByField(n, gs.fieldObject);
            const TSNode propNode = parser::GetChildByField(n, gs.fieldMember);
            if (!ts_node_is_null(objNode) && !ts_node_is_null(propNode) && parser::NodeText(objNode, source) == "this")
            {
                if (!parser::NodeText(propNode, source).empty())
                {
                    refs.push_back(IndexedMixinMemberRef{MakeIndexedSourceSpan(propNode), MixinRefKind::ThisMember});
                }
            }
        });
    ForEachBoundedNode(index.Nodes(gs.symCallExpression), startByte, endByte,
                       [&](TSNode n)
                       {
                           const TSNode fnNode = parser::GetChildByField(n, gs.fieldFunction);
                           if (!ts_node_is_null(fnNode))
                           {
                               const TSSymbol sym = ts_node_symbol(fnNode);
                               if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
                               {
                                   const std::string_view name = parser::NodeText(fnNode, source);
                                   if (!name.empty() && !utils::ContainsScopeSeparator(name))
                                   {
                                       refs.push_back(
                                           IndexedMixinMemberRef{MakeIndexedSourceSpan(fnNode), MixinRefKind::Call});
                                   }
                               }
                           }
                       });
}

/**
 * @brief Extracts AST-free member references and function declarations from a mixin body.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] source Document source text buffer.
 * @param[in] mixinPos 0-based source coordinate of the mixin declaration symbol.
 * @return Extracted mixin body facts; found=false when no declaration body exists.
 */
MixinBodyFacts BuildMixinBodyFacts(const NodeIndex& index, std::string_view source, IndexedSourcePoint mixinPos)
{
    const TSNode root = index.Root();
    if (ts_node_is_null(root))
    {
        return {};
    }
    const TSNode mixinNode = FindMixinAstNode(root, mixinPos);
    if (ts_node_is_null(mixinNode))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode bodyNode = parser::GetChildByField(mixinNode, gs.fieldBody);
    if (ts_node_is_null(bodyNode))
    {
        return {};
    }

    MixinBodyFacts facts{};
    facts.found = true;
    parser::ForEachChildNode(bodyNode,
                             [&](TSNode mem)
                             {
                                 if (ts_node_symbol(mem) != gs.symFuncDeclaration)
                                 {
                                     return;
                                 }
                                 const TSNode funcBody = parser::GetChildByField(mem, gs.fieldBody);
                                 if (ts_node_is_null(funcBody))
                                 {
                                     return;
                                 }
                                 IndexedMixinFunction func{};
                                 CollectLocalNames(mem, funcBody, index, func.localNames);
                                 AppendMemberRefs(funcBody, index, source, func.refs);
                                 AppendMixinIdentifierRefs(
                                     index, IndexedByteRange{ts_node_start_byte(funcBody), ts_node_end_byte(funcBody)},
                                     func.refs);
                                 facts.functions.push_back(std::move(func));
                             });
    return facts;
}

} // namespace angel_lsp::analysis
