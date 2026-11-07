#include "analysis/index/HoverFactsInternal.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"

#include <cctype>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{
bool IsHoverableIdentifier(std::string_view txt, TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (txt == "?" || sym == gs.symDatatype || sym == gs.symIdentifier || sym == gs.symScopedIdentifier ||
        sym == gs.symPrimitiveType || sym == gs.symStringLiteral || analysis::IsPrimitiveTypeName(txt) ||
        analysis::IsReservedKeyword(std::string(txt)))
    {
        return true;
    }

    if (!txt.empty() && (isalpha(static_cast<unsigned char>(txt[0])) || txt[0] == '_'))
    {
        for (char c : txt)
        {
            if (!isalnum(static_cast<unsigned char>(c)) && c != '_')
            {
                return false;
            }
        }
        return true;
    }
    return false;
}

static TSNode TryExtractHoverTargetNode(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const uint32_t sb = ts_node_start_byte(node);
    const uint32_t eb = ts_node_end_byte(node);
    if (sb >= sourceCode.size() || eb > sourceCode.size() || sb >= eb)
    {
        return TSNode{};
    }

    const std::string_view txt = sourceCode.substr(sb, eb - sb);
    if (!IsHoverableIdentifier(txt, ts_node_symbol(node)))
    {
        return TSNode{};
    }
    return node;
}

TSNode ExtractHoverNode(TSNode rootNode, std::string_view sourceCode, TSPoint point)
{
    TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }

    if (TSNode target = TryExtractHoverTargetNode(node, sourceCode); !ts_node_is_null(target))
    {
        return target;
    }

    if (TSNode target = TryExtractHoverTargetNode(parser::GetParent(node), sourceCode); !ts_node_is_null(target))
    {
        return target;
    }

    if (point.column > 0)
    {
        TSPoint prevPt = {point.row, point.column - 1};
        TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPt, prevPt);
        if (TSNode target = TryExtractHoverTargetNode(prevNode, sourceCode); !ts_node_is_null(target))
        {
            return target;
        }
        if (!ts_node_is_null(prevNode))
        {
            if (TSNode target = TryExtractHoverTargetNode(parser::GetParent(prevNode), sourceCode);
                !ts_node_is_null(target))
            {
                return target;
            }
        }
    }

    return TSNode{};
}

static TSNode FindCallFromParent(TSNode parent, TSNode node)
{
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symCallExpression)
    {
        TSNode fn = parser::GetChildByField(parent, gs.fieldFunction);
        if (!ts_node_is_null(fn) && (ts_node_eq(fn, node) || ts_node_start_byte(fn) == ts_node_start_byte(node)))
        {
            return parent;
        }
    }
    else if (pType == gs.symConstructCallExpression)
    {
        TSNode typeChild = parser::GetChildByField(parent, gs.fieldType);
        if (!ts_node_is_null(typeChild) &&
            (ts_node_eq(typeChild, node) || ts_node_start_byte(typeChild) == ts_node_start_byte(node)))
        {
            return parent;
        }
    }
    return TSNode{};
}

TSNode FindEnclosingCallNode(TSNode node)
{
    TSNode parent = parser::GetParent(node);
    if (ts_node_is_null(parent))
    {
        return TSNode{};
    }
    if (TSNode call = FindCallFromParent(parent, node); !ts_node_is_null(call))
    {
        return call;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pType = ts_node_symbol(parent);
    if (pType == gs.symDatatype || pType == gs.symMemberExpression || pType == gs.symScopedIdentifier)
    {
        TSNode grandParent = parser::GetParent(parent);
        if (!ts_node_is_null(grandParent))
        {
            if (TSNode call = FindCallFromParent(grandParent, parent); !ts_node_is_null(call))
            {
                return call;
            }
        }
    }
    return TSNode{};
}

bool IsDirectInitDeclarator(TSNode node)
{
    TSNode declarator = node;
    if (!ts_node_is_null(declarator) && ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        declarator = parser::GetParent(node);
    }
    if (ts_node_is_null(declarator) || ts_node_symbol(declarator) != parser::GetGrammarSymbols().symVariableDeclarator)
    {
        return false;
    }
    TSNode args = parser::GetChildByField(declarator, parser::GetGrammarSymbols().fieldArguments);
    return !ts_node_is_null(args);
}
} // namespace angel_lsp::analysis
