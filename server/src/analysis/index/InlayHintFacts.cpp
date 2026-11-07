#include "analysis/index/InlayHintFacts.h"

#include "analysis/index/InlayHintFactsInternal.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

static void ProcessCallNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                            InlayHintSites& sites)
{
    const auto target = ExtractCallTargetSpans(node, gs);
    if (ts_node_is_null(target.funcNode))
    {
        return;
    }

    IndexedCallSite site;
    site.callRange = ToSourceRange(node);
    site.startByte = ts_node_start_byte(node);
    site.endByte = ts_node_end_byte(node);
    site.calleeStartByte = ts_node_start_byte(target.funcNode);
    site.calleeEndByte = ts_node_end_byte(target.funcNode);
    site.calleeText = utils::SliceSource(sourceCode, site.calleeStartByte, site.calleeEndByte);
    site.isMemberCall = target.isMember;
    site.receiverStartByte = target.receiverStartByte;
    site.receiverEndByte = target.receiverEndByte;

    TSNode argList = ResolveArgumentListNode(node);
    site.hasArgumentList = !ts_node_is_null(argList);
    PopulateArgListCoords(argList, site.emptyArgsLine, site.emptyArgsCharacter, site.closeParenLine,
                          site.closeParenCharacter);
    site.firstArgument = static_cast<uint32_t>(sites.callArguments.size());
    site.argumentCount = ExtractArguments(argList, sourceCode, sites);
    sites.calls.push_back(site);
}

static void ProcessConstructCallNode(TSNode node, std::string_view sourceCode, const parser::GrammarSymbols& gs,
                                     InlayHintSites& sites)
{
    TSNode typeNode = parser::GetChildByField(node, gs.fieldType);
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    IndexedCallSite site;
    site.callRange = ToSourceRange(node);
    site.startByte = ts_node_start_byte(node);
    site.endByte = ts_node_end_byte(node);
    site.isConstructorCall = true;

    uint32_t tStart = ts_node_start_byte(typeNode);
    uint32_t tEnd = ts_node_end_byte(typeNode);
    parser::ForEachChildNodeUntil(node,
                                  [&](TSNode child)
                                  {
                                      if (ts_node_symbol(child) == gs.symTemplateTypeList)
                                      {
                                          tEnd = ts_node_end_byte(child);
                                          return false;
                                      }
                                      return true;
                                  });
    site.constructTypeName = utils::SliceSource(sourceCode, tStart, tEnd);

    TSNode argList = ResolveArgumentListNode(node);
    site.hasArgumentList = !ts_node_is_null(argList);
    PopulateArgListCoords(argList, site.emptyArgsLine, site.emptyArgsCharacter, site.closeParenLine,
                          site.closeParenCharacter);
    site.firstArgument = static_cast<uint32_t>(sites.callArguments.size());
    site.argumentCount = ExtractArguments(argList, sourceCode, sites);
    sites.calls.push_back(site);
}

InlayHintSites CollectInlayHintSites(const NodeIndex& index, uint32_t startLine, uint32_t endLine,
                                     std::string_view sourceCode)
{
    InlayHintSites sites;
    const auto& gs = parser::GetGrammarSymbols();
    const std::array<TSSymbol, 5> targetSymbols = {
        gs.symCallExpression,   gs.symConstructCallExpression, gs.symVariableDeclaration,
        gs.symLambdaExpression, gs.symEnumDeclaration,
    };

    index.ForEachNodeOrdered(targetSymbols,
                             [&](TSNode current)
                             {
                                 if (!IsNodeOverlapping(current, startLine, endLine))
                                 {
                                     return;
                                 }
                                 const TSSymbol sym = ts_node_symbol(current);
                                 if (sym == gs.symCallExpression)
                                 {
                                     ProcessCallNode(current, sourceCode, gs, sites);
                                 }
                                 else if (sym == gs.symConstructCallExpression)
                                 {
                                     ProcessConstructCallNode(current, sourceCode, gs, sites);
                                 }
                                 else if (sym == gs.symVariableDeclaration)
                                 {
                                     ProcessVariableDeclarationNode(current, sourceCode, gs, sites);
                                 }
                                 else if (sym == gs.symLambdaExpression)
                                 {
                                     ProcessLambdaNode(current, sourceCode, gs, sites);
                                 }
                                 else if (sym == gs.symEnumDeclaration)
                                 {
                                     ProcessEnumNode(current, sourceCode, gs, sites);
                                 }
                             });

    return sites;
}

} // namespace angel_lsp::analysis
