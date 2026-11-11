#include "analysis/resolution/ScopeLookupInternal.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/index/SyntaxOperands.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/QueryRegistry.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

TSNode AdjustStartNodeIfDeclarationName(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode result = node;
    parser::ForEachAncestor(
        node,
        [&](TSNode cur) -> bool
        {
            const TSNode parent = parser::GetParent(cur);
            if (ts_node_is_null(parent))
            {
                return false;
            }
            const TSSymbol parentSym = ts_node_symbol(parent);
            if (parentSym == gs.symBaseClassList)
            {
                TSNode decl = parser::GetParent(parent);
                if (!ts_node_is_null(decl))
                {
                    result = decl;
                    return false;
                }
            }
            if (parentSym == gs.symClassDeclaration || parentSym == gs.symInterfaceDeclaration ||
                parentSym == gs.symNamespaceDeclaration || parentSym == gs.symMixinDeclaration)
            {
                const TSNode nameChild = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldName);
                if (!ts_node_is_null(nameChild) && ts_node_start_byte(nameChild) == ts_node_start_byte(node) &&
                    ts_node_end_byte(nameChild) == ts_node_end_byte(node))
                {
                    result = parent;
                    return false;
                }
            }
            return true;
        });
    return result;
}

std::vector<ContainerInfo> GetEnclosingContainers(TSNode node, std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    if (ts_node_is_null(node))
    {
        return {};
    }

    if (nodeIndex)
    {
        return nodeIndex->CollectLookupContainers(ts_node_start_byte(node), ts_node_end_byte(node),
                                                  ts_node_symbol(node), sourceCode);
    }

    std::vector<ContainerInfo> rawContainers;
    bool crossedLambda = false;
    const auto& gs = parser::GetGrammarSymbols();
    parser::ForEachAncestor(
        parser::GetParent(AdjustStartNodeIfDeclarationName(node)),
        [&](TSNode current)
        {
            if (ts_node_symbol(current) == gs.symLambdaExpression)
            {
                crossedLambda = true;
            }

            if (const auto kind = ClassifyContainerKind(ts_node_symbol(current)))
            {
                if (crossedLambda && (*kind == ContainerKind::Class || *kind == ContainerKind::Interface))
                {
                    return;
                }

                AppendRawContainers(parser::GetChildByField(current, parser::GetGrammarSymbols().fieldName), *kind,
                                    sourceCode, rawContainers);
            }
        });

    ComputeContainerQualifiedNames(rawContainers);
    return rawContainers;
}

std::vector<std::string> CollectUsingNamespaces(TSNode root, std::string_view sourceCode)
{
    std::vector<std::string> usings;
    if (ts_node_is_null(root) || sourceCode.find("using") == std::string_view::npos)
    {
        return usings;
    }

    const TSQuery* query = parser::QueryRegistry::GetUsingQuery();
    TSQueryCursor* cursor = parser::QueryRegistry::GetThreadLocalCursor();
    if (!query || !cursor)
    {
        return usings;
    }

    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint16_t i = 0; i < match.capture_count; ++i)
        {
            TSNode nameNode = match.captures[i].node;
            std::string uName(parser::NodeText(nameNode, sourceCode));
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                uName.erase(uName.begin());
            while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                uName.pop_back();
            if (!uName.empty())
            {
                usings.push_back(std::move(uName));
            }
        }
    }
    return usings;
}

static void CollectUsingsFromAncestor(TSNode p, std::string_view sourceCode, std::vector<std::string>& usings)
{
    TSTreeCursor cursor = ts_tree_cursor_new(p);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode ch = ts_tree_cursor_current_node(&cursor);
            const auto& gs = parser::GetGrammarSymbols();
            if (ts_node_symbol(ch) == gs.symUsingDeclaration)
            {
                TSNode nameNode = parser::GetChildByField(ch, gs.fieldName);
                if (!ts_node_is_null(nameNode))
                {
                    std::string uName(parser::NodeText(nameNode, sourceCode));
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.front())))
                        uName.erase(uName.begin());
                    while (!uName.empty() && isspace(static_cast<unsigned char>(uName.back())))
                        uName.pop_back();
                    if (!uName.empty())
                    {
                        usings.push_back(uName);
                    }
                }
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

std::vector<std::string> CollectVisibleUsingNamespaces(TSNode node, std::string_view sourceCode,
                                                       const NodeIndex* nodeIndex)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    if (nodeIndex)
    {
        return CollectVisibleUsingNamespaces(ts_node_start_byte(node), sourceCode, nodeIndex);
    }

    std::vector<std::string> usings;
    if (sourceCode.find("using") != std::string_view::npos)
    {
        parser::ForEachAncestor(node, [&](TSNode p) { CollectUsingsFromAncestor(p, sourceCode, usings); });
    }
    return usings;
}

std::vector<Symbol> FindSymbolsInScope(const std::string& name, TSNode node, std::string_view sourceCode,
                                       const SymbolTable& symbolTable, const NodeIndex* nodeIndex)
{
    if (name.empty())
    {
        return {};
    }
    if (nodeIndex && !ts_node_is_null(node))
    {
        return FindSymbolsInScope(name, ts_node_start_byte(node), sourceCode, symbolTable, nodeIndex);
    }
    return FindSymbolsInScope(symbolTable, GetEnclosingContainers(node, sourceCode, nodeIndex), name,
                              CollectVisibleUsingNamespaces(node, sourceCode, nodeIndex));
}

static std::string ExtractTrimmedIdentifier(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return {};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return {};
    }
    std::string name(parser::NodeText(node, sourceCode));
    while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
    {
        name.erase(name.begin());
    }
    while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
    {
        name.pop_back();
    }
    return name;
}

std::optional<std::string> IsBareDataType(TSNode node, const ExpressionTypeContext& ctx)
{
    node = UnwrapIndexedParentheses(node, ctx.nodeIndex);
    std::string name = ExtractTrimmedIdentifier(node, ctx.sourceCode);
    if (name.empty())
    {
        return std::nullopt;
    }
    return IsBareDataType(name, ts_node_start_byte(node), ctx);
}

} // namespace angel_lsp::analysis
