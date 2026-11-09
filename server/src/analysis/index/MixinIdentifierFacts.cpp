#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/EngineDialectFacts.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Checks if a child node matches a named field of the parent by byte offset.
 * @param[in] parent Enclosing parent AST node.
 * @param[in] fieldId Grammar field identifier to inspect.
 * @param[in] cur Target child AST node.
 * @return True if the named child exists and starts at the same byte as cur; false otherwise.
 */
static bool IsNamedChildMatch(TSNode parent, TSFieldId fieldId, TSNode cur)
{
    if (ts_node_is_null(parent) || ts_node_is_null(cur))
    {
        return false;
    }
    const TSNode child = ts_node_child_by_field_id(parent, fieldId);
    return !ts_node_is_null(child) && ts_node_start_byte(child) == ts_node_start_byte(cur);
}

/**
 * @brief Checks if a scoped identifier context represents an external or qualified lookup.
 * @param[in] parent Parent scoped_identifier AST node.
 * @param[in] gs Precompiled grammar symbol table.
 * @return True if the scoped identifier should be ignored; false otherwise.
 */
static bool IsScopedIdentifierIgnored(TSNode parent, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(parent))
    {
        return false;
    }
    if (ts_node_child_count(parent) > 1)
    {
        return true;
    }
    const TSNode grandparent = parser::GetParent(parent);
    if (ts_node_is_null(grandparent))
    {
        return false;
    }
    const TSSymbol gpSym = ts_node_symbol(grandparent);
    if (gpSym == gs.symScope || gpSym == gs.symUsingDeclaration)
    {
        return true;
    }
    return gpSym == gs.symCallExpression && IsNamedChildMatch(grandparent, gs.fieldFunction, parent);
}

/**
 * @brief Checks if an identifier is a declaration name or member field name.
 * @param[in] cur Candidate identifier AST node.
 * @param[in] parent Enclosing parent AST node.
 * @param[in] pSym Grammar symbol type of the parent node.
 * @param[in] gs Precompiled grammar symbol table.
 * @return True if cur is the name of a declaration or field access; false otherwise.
 */
static bool IsDeclarationOrFieldChild(TSNode cur, TSNode parent, TSSymbol pSym, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(cur) || ts_node_is_null(parent))
    {
        return false;
    }
    if (pSym == gs.symMemberExpression)
    {
        return IsNamedChildMatch(parent, gs.fieldMember, cur);
    }
    if (pSym == gs.symCallExpression)
    {
        return IsNamedChildMatch(parent, gs.fieldFunction, cur);
    }
    if (pSym == gs.symVariableDeclarator || pSym == gs.symParameter || pSym == gs.symFuncDeclaration ||
        pSym == gs.symForeachVariable || pSym == gs.symLambdaParameterList)
    {
        return IsNamedChildMatch(parent, gs.fieldName, cur);
    }
    if (pSym == gs.symArgumentList)
    {
        return IsNamedChildMatch(parent, gs.fieldArgName, cur);
    }
    if (pSym == gs.symIndexExpression)
    {
        return IsNamedChildMatch(parent, gs.fieldIndexName, cur);
    }
    return false;
}

/**
 * @brief Determines if an identifier node is in a non-reference AST context.
 * @param[in] cur Candidate identifier AST node.
 * @return True if the identifier should be ignored for mixin member analysis; false otherwise.
 */
static bool ShouldIgnoreMixinIdentifier(TSNode cur)
{
    if (ts_node_is_null(cur))
    {
        return true;
    }
    const TSNode parent = parser::GetParent(cur);
    if (ts_node_is_null(parent))
    {
        return true;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol pSym = ts_node_symbol(parent);
    if (pSym == gs.symScopedIdentifier)
    {
        return IsScopedIdentifierIgnored(parent, gs);
    }

    if (IsDeclarationOrFieldChild(cur, parent, pSym, gs))
    {
        return true;
    }

    if (pSym == gs.symVariableDeclaration)
    {
        return IsNamedChildMatch(parent, gs.fieldVarType, cur);
    }

    if (pSym == gs.symParameter)
    {
        return IsNamedChildMatch(parent, gs.fieldType, cur) || IsNamedChildMatch(parent, gs.fieldParamType, cur);
    }

    return pSym == gs.symType || pSym == gs.symPrimitiveType || pSym == gs.symCastExpression ||
           pSym == gs.symDatatype || pSym == gs.symNestedTypeName;
}

/**
 * @brief Appends unqualified identifier references occurring within a mixin function body.
 * @param[in] index Node index of the current syntax revision.
 * @param[in] body Byte range of the function body.
 * @param[out] refs Destination collection for mixin member references appended in start-byte order.
 */
void AppendMixinIdentifierRefs(const NodeIndex& index, IndexedByteRange body, std::vector<IndexedMixinMemberRef>& refs)
{
    const auto& gs = parser::GetGrammarSymbols();
    const std::span<const TSNode> nodes = index.Nodes(gs.symIdentifier);
    auto it = std::lower_bound(nodes.begin(), nodes.end(), body.startByte,
                               [](const TSNode& n, uint32_t b) { return ts_node_start_byte(n) < b; });
    for (; it != nodes.end() && ts_node_start_byte(*it) < body.endByte; ++it)
    {
        if (ts_node_end_byte(*it) <= body.endByte)
        {
            if (!ShouldIgnoreMixinIdentifier(*it))
            {
                refs.push_back(IndexedMixinMemberRef{MakeIndexedSourceSpan(*it), MixinRefKind::Identifier});
            }
        }
    }
}

} // namespace angel_lsp::analysis
