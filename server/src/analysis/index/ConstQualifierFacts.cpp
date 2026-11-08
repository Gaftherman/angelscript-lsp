#include "analysis/index/ConstQualifierFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
static TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::GetGrammarSymbols().fieldBody);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    return parser::FindFirstChildOfType(fnNode, "statement_block");
}

/**
 * @brief Finds the parameter list node of a function declaration.
 * @param[in] fnNode Function declaration node.
 * @return Parameter list node or null node.
 */
static TSNode FindFunctionParametersNode(TSNode fnNode)
{
    TSNode paramList = parser::GetChildByField(fnNode, parser::GetGrammarSymbols().fieldParameters);
    if (!ts_node_is_null(paramList))
    {
        return paramList;
    }
    return parser::FindFirstChildOfType(fnNode, "parameter_list");
}

/**
 * @brief Checks whether the 'const' keyword appears in source text between the parameter list and function body.
 * @param[in] sourceCode Document source text buffer.
 * @param[in] paramList AST node of the function parameter list.
 * @param[in] bodyNode AST node of the function compound body (or null if abstract).
 * @param[in] fnNode AST node of the enclosing function declaration.
 * @return True if 'const' token is found in the intervening source span, false otherwise.
 */
static bool HasConstQualifierBetween(TSNode fnNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode attr = parser::FindChildWithSymbol(fnNode, gs.symFuncAttributes);
    if (!ts_node_is_null(attr) && !ts_node_is_null(parser::FindChildWithSymbol(attr, gs.tokConst)))
    {
        return true;
    }
    return !ts_node_is_null(parser::FindChildWithSymbol(fnNode, gs.tokConst));
}

static bool FindEnclosingMethodAndClass(TSNode leaf, TSNode& outFn, TSNode& outClass, const parser::GrammarSymbols& gs)
{
    TSNode fnNode = parser::FindAncestorWithSymbol(leaf, gs.symFuncDeclaration);
    TSNode classNode = parser::FindAncestorWithSymbol(fnNode, gs.symClassDeclaration);
    if (ts_node_is_null(fnNode) || ts_node_is_null(classNode))
    {
        return false;
    }
    outFn = fnNode;
    outClass = classNode;
    return true;
}

IndexedConstCallSite FindConstCallSite(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return IndexedConstCallSite{};
    }

    const TSPoint pt{line, column};
    const TSNode memberNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(memberNode))
    {
        return IndexedConstCallSite{};
    }

    const TSNode callee = parser::GetParent(memberNode);
    const TSNode objNode = parser::GetChildByField(callee, parser::GetGrammarSymbols().fieldObject);

    const IndexedByteRange memberRange{ts_node_start_byte(memberNode), ts_node_end_byte(memberNode)};
    const IndexedByteRange objRange = ts_node_is_null(objNode)
                                          ? IndexedByteRange{0, 0}
                                          : IndexedByteRange{ts_node_start_byte(objNode), ts_node_end_byte(objNode)};

    return IndexedConstCallSite{memberRange, objRange};
}

std::optional<IndexedSourcePoint> FindMethodParameterListEnd(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSPoint pt{line, column};
    const TSNode fnNode =
        parser::FindAncestorWithSymbol(ts_node_descendant_for_point_range(rootNode, pt, pt), gs.symFuncDeclaration);
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }

    const TSNode paramList = FindFunctionParametersNode(fnNode);
    if (ts_node_is_null(paramList))
    {
        return std::nullopt;
    }

    const TSPoint insertPt = ts_node_end_point(paramList);
    return IndexedSourcePoint{insertPt.row, insertPt.column};
}

std::optional<IndexedIntentionalConstSite> FindIntentionalConstSite(const NodeIndex& index, std::string_view sourceCode,
                                                                    uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const TSPoint pt{line, column};
    const TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    TSNode fnNode = {};
    TSNode classNode = {};
    if (!FindEnclosingMethodAndClass(leaf, fnNode, classNode, gs))
    {
        return std::nullopt;
    }

    const TSNode paramList = FindFunctionParametersNode(fnNode);
    const TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(paramList) || ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }
    if (HasConstQualifierBetween(fnNode))
    {
        return std::nullopt;
    }

    IndexedIntentionalConstSite site;
    const TSPoint insertPt = ts_node_end_point(paramList);
    site.parameterListEnd = IndexedSourcePoint{insertPt.row, insertPt.column};
    site.methodStartLine = ts_node_start_point(fnNode).row;

    const TSNode classNameNode = parser::GetChildByField(classNode, gs.fieldName);
    site.className = ts_node_is_null(classNameNode)
                         ? IndexedByteRange{0, 0}
                         : IndexedByteRange{ts_node_start_byte(classNameNode), ts_node_end_byte(classNameNode)};

    CollectMethodMutationFacts(bodyNode, classNode, sourceCode, site.mutation);
    return site;
}

std::optional<IndexedConstHandleSite> FindConstHandleSite(const NodeIndex& index, uint32_t line, uint32_t column)
{
    const TSNode rootNode = index.Root();
    if (ts_node_is_null(rootNode))
    {
        return std::nullopt;
    }

    const TSPoint pt{line, column};
    const TSNode node = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(node))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode declNode = parser::FindAncestorWithSymbol(node, gs.symVariableDeclaration);
    if (ts_node_is_null(declNode))
    {
        return std::nullopt;
    }

    TSNode typeNode = parser::GetChildByField(declNode, gs.fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::FindChildWithSymbol(declNode, gs.symType);
    }
    if (ts_node_is_null(typeNode))
    {
        return std::nullopt;
    }

    const TSPoint insertPt = ts_node_start_point(typeNode);
    return IndexedConstHandleSite{
        IndexedByteRange{ts_node_start_byte(typeNode), ts_node_end_byte(typeNode)},
        IndexedSourcePoint{insertPt.row, insertPt.column},
    };
}

} // namespace angel_lsp::analysis
