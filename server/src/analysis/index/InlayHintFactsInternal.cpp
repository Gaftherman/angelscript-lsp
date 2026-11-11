#include "analysis/index/InlayHintFactsInternal.h"

#include "analysis/index/ReferenceReceiverIndex.h"
#include "analysis/index/CallArgumentFacts.h"
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
        arg.text = utils::SliceSource(sourceCode, arg.startByte, arg.endByte);
        if (!ts_node_is_null(a.nameNode))
        {
            arg.name = utils::SliceSource(sourceCode, ts_node_start_byte(a.nameNode), ts_node_end_byte(a.nameNode));
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

CallTargetSpans ExtractTargetSpansFromFunctionNode(TSNode funcNode, const parser::GrammarSymbols& gs) noexcept
{
    CallTargetSpans spans;
    if (ts_node_is_null(funcNode))
    {
        return spans;
    }

    spans.funcNode = funcNode;
    spans.calleeStartByte = ts_node_start_byte(funcNode);
    spans.calleeEndByte = ts_node_end_byte(funcNode);

    const TSSymbol funcSym = ts_node_symbol(funcNode);
    if (funcSym == gs.symMemberExpression)
    {
        spans.isMember = true;
        TSNode memberNode = parser::GetChildByField(funcNode, gs.fieldMember);
        TSNode objNode = parser::GetChildByField(funcNode, gs.fieldObject);
        if (!ts_node_is_null(memberNode))
        {
            spans.calleeStartByte = ts_node_start_byte(memberNode);
            spans.calleeEndByte = ts_node_end_byte(memberNode);
        }
        if (!ts_node_is_null(objNode))
        {
            spans.receiverStartByte = ts_node_start_byte(objNode);
            spans.receiverEndByte = ts_node_end_byte(objNode);
        }
    }
    else if (funcSym == gs.symScopedIdentifier)
    {
        const TSNode nameNode = ScopedIdentifierTail(funcNode);
        if (!ts_node_is_null(nameNode))
        {
            spans.calleeStartByte = ts_node_start_byte(nameNode);
            spans.calleeEndByte = ts_node_end_byte(nameNode);
            if (ts_node_start_byte(nameNode) > ts_node_start_byte(funcNode))
            {
                spans.isQualified = true;
                spans.qualifierStartByte = ts_node_start_byte(funcNode);
                const uint32_t namedCount = ts_node_named_child_count(funcNode);
                spans.qualifierEndByte = namedCount > 1
                                             ? ts_node_end_byte(ts_node_named_child(funcNode, namedCount - 2))
                                             : spans.qualifierStartByte;
            }
        }
    }

    return spans;
}

CallTargetSpans ExtractCallTargetSpans(TSNode callNode, const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(callNode))
    {
        return {};
    }

    TSNode funcChild = parser::GetChildByField(callNode, gs.fieldFunction);
    if (ts_node_is_null(funcChild) && ts_node_named_child_count(callNode) > 0)
    {
        funcChild = ts_node_named_child(callNode, 0);
    }
    return ExtractTargetSpansFromFunctionNode(funcChild, gs);
}

} // namespace angel_lsp::analysis
