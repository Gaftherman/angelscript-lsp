#include "analysis/index/ConstQualifierFacts.h"

#include "analysis/index/CodeActionFactsInternal.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"

#include <algorithm>
#include <iterator>
#include <tree_sitter/api.h>
#include <tuple>

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
    return parser::FindChildWithSymbol(fnNode, parser::GetGrammarSymbols().symStatementBlock);
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
    return parser::FindChildWithSymbol(fnNode, parser::GetGrammarSymbols().symParameterList);
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
    const TSPoint startPt = ts_node_start_point(fnNode);
    const TSPoint insertPt = ts_node_end_point(paramList);
    const auto cursor = std::tie(pt.row, pt.column);
    if (cursor < std::tie(startPt.row, startPt.column) || cursor >= std::tie(insertPt.row, insertPt.column) ||
        HasConstQualifierBetween(fnNode))
    {
        return std::nullopt;
    }

    IndexedIntentionalConstSite site;
    site.parameterListEnd = IndexedSourcePoint{insertPt.row, insertPt.column};
    site.methodStartLine = startPt.row;

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

ParameterRefFacts BuildParameterRefFacts(const NodeIndex& index, std::string_view source)
{
    const auto point = [](TSPoint p) { return IndexedSourcePoint{p.row, p.column}; };
    const auto& grammar = parser::GetGrammarSymbols();
    ParameterRefFacts result;
    for (const TSNode parameter : index.Nodes(grammar.symParameter))
    {
        if (ts_node_is_null(parameter))
        {
            continue;
        }
        const TSNode amp = parser::FindChildWithSymbol(parameter, grammar.tokAmp);
        const TSNode direction = parser::GetChildByField(parameter, grammar.fieldDirection);
        const bool bare = ts_node_is_null(direction);
        if (ts_node_is_null(amp) || (!bare && ts_node_symbol(direction) != grammar.tokInout))
        {
            continue;
        }

        const TSNode type = parser::GetChildByField(parameter, grammar.fieldParamType);
        const TSNode defaultValue = parser::GetChildByField(parameter, grammar.fieldDefaultValue);
        const TSNode name = parser::GetChildByField(parameter, grammar.fieldName);
        const bool isConst =
            !ts_node_is_null(type) && !ts_node_is_null(parser::FindChildWithSymbol(type, grammar.tokConst));
        const bool hasDefault =
            !ts_node_is_null(defaultValue) && !parser::primitives::IsVoidType(parser::NodeText(defaultValue, source));
        const TSPoint editEnd = ts_node_end_point(bare ? amp : direction);
        result.sites.push_back(IndexedParameterRefSite{
            .start = point(ts_node_start_point(parameter)),
            .end = point(ts_node_end_point(parameter)),
            .editStart = point(bare ? editEnd : ts_node_start_point(direction)),
            .editEnd = point(editEnd),
            .needsSeparator = bare && !ts_node_is_null(name) && ts_node_start_byte(name) == ts_node_end_byte(amp),
            .allowsOutput = !isConst && !hasDefault,
        });
    }
    return result;
}

const IndexedParameterRefSite* ParameterRefFacts::FindAt(uint32_t line, uint32_t column) const noexcept
{
    const auto position = std::tie(line, column);
    const auto past = std::partition_point(sites.begin(), sites.end(), [&](const IndexedParameterRefSite& site)
                                           { return std::tie(site.start.line, site.start.character) <= position; });
    if (past == sites.begin())
    {
        return nullptr;
    }
    const auto& site = *std::prev(past);
    return position < std::tie(site.end.line, site.end.character) ? &site : nullptr;
}

} // namespace angel_lsp::analysis
