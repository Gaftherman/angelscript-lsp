#include "analysis/index/InlayHintFactsInternal.h"

#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

void PopulateArgListCoords(TSNode listNode, uint32_t& emptyLine, uint32_t& emptyCol, uint32_t& closeLine,
                           uint32_t& closeCol) noexcept
{
    if (ts_node_is_null(listNode))
    {
        return;
    }
    const TSPoint emptyPt = (ts_node_child_count(listNode) > 0) ? ts_node_end_point(ts_node_child(listNode, 0))
                                                                : ts_node_start_point(listNode);
    emptyLine = emptyPt.row;
    emptyCol = emptyPt.column;
    const TSPoint closePt = ts_node_end_point(listNode);
    closeLine = closePt.row;
    closeCol = closePt.column;
}

uint32_t ExtractArguments(TSNode listNode, std::string_view sourceCode, InlayHintSites& sites)
{
    if (ts_node_is_null(listNode))
    {
        return 0;
    }
    const auto callArgs = ExtractCallArguments(listNode, sourceCode);
    for (const auto& a : callArgs)
    {
        if (ts_node_is_null(a.exprNode))
        {
            continue;
        }
        IndexedCallArgument arg;
        arg.range = ToSourceRange(a.exprNode);
        arg.startByte = ts_node_start_byte(a.exprNode);
        arg.endByte = ts_node_end_byte(a.exprNode);
        const TSPoint pt = ts_node_start_point(a.exprNode);
        arg.hintLine = pt.row;
        arg.hintCharacter = pt.column;
        arg.text = SliceSource(sourceCode, arg.startByte, arg.endByte);
        if (!ts_node_is_null(a.nameNode))
        {
            arg.name = SliceSource(sourceCode, ts_node_start_byte(a.nameNode), ts_node_end_byte(a.nameNode));
            const TSPoint namePt = ts_node_start_point(a.nameNode);
            arg.argStartLine = namePt.row;
            arg.argStartCharacter = namePt.column;
        }
        else
        {
            arg.argStartLine = pt.row;
            arg.argStartCharacter = pt.column;
        }
        sites.callArguments.push_back(arg);
    }
    return static_cast<uint32_t>(callArgs.size());
}

} // namespace angel_lsp::analysis
