/**
 * @file CodeActionExtractMethodStatements.cpp
 * @brief Analysis for statement range selection and input variable identification.
 */

#include "features/code_action/CodeActionExtractMethodAnalysis.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Finds the enclosing function node for a selection range.
 * @param[in] rootNode AST root node.
 * @param[in] range Selection range.
 * @return Function declaration node or null node.
 */
TSNode FindEnclosingFunctionNode(TSNode rootNode, const lsp::Range& range)
{
    TSPoint startPt = {range.start.line, range.start.character};
    TSPoint endPt = {range.end.line, range.end.character};
    TSNode selNode = ts_node_descendant_for_point_range(rootNode, startPt, endPt);
    TSNode fnNode = selNode;
    while (!ts_node_is_null(fnNode) && std::string_view(ts_node_type(fnNode)) != "func_declaration")
    {
        fnNode = ts_node_parent(fnNode);
    }
    return fnNode;
}

/**
 * @brief Finds the statement block body of a function node.
 * @param[in] fnNode Function declaration node.
 * @return Statement block node or null node.
 */
TSNode FindFunctionBodyBlock(TSNode fnNode)
{
    TSNode bodyNode = parser::GetChildByField(fnNode, parser::fields::Body);
    if (!ts_node_is_null(bodyNode))
    {
        return bodyNode;
    }
    uint32_t cnt = ts_node_child_count(fnNode);
    for (uint32_t i = 0; i < cnt; ++i)
    {
        TSNode ch = ts_node_child(fnNode, i);
        if (std::string_view(ts_node_type(ch)) == "statement_block")
        {
            return ch;
        }
    }
    return bodyNode;
}

/**
 * @brief Collects statements within a block that intersect the selection range.
 * @param[in] bodyNode Statement block node.
 * @param[in] range Selection range.
 * @return Vector of intersecting statement nodes.
 */
std::vector<TSNode> CollectStatementsInRange(TSNode bodyNode, const lsp::Range& range)
{
    std::vector<TSNode> selectedStmts;
    uint32_t childCount = ts_node_child_count(bodyNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode ch = ts_node_child(bodyNode, i);
        if (!ts_node_is_named(ch))
        {
            continue;
        }
        std::string_view t = ts_node_type(ch);
        if (t == "{" || t == "}")
        {
            continue;
        }
        TSPoint cStart = ts_node_start_point(ch);
        TSPoint cEnd = ts_node_end_point(ch);
        if (cStart.row <= range.end.line && cEnd.row >= range.start.line)
        {
            selectedStmts.push_back(ch);
        }
    }
    return selectedStmts;
}

/**
 * @brief Checks if a local definition is declared inside the statement range.
 * @param[in] def Local definition pointer.
 * @param[in] stmts Selected statement information.
 * @return True if declared inside the selection.
 */
bool IsDeclaredInsideRange(const analysis::LocalDefinition* def, const ExtractMethodStatements& stmts)
{
    bool afterStart = def->startLine > stmts.firstStart.row ||
                      (def->startLine == stmts.firstStart.row && def->startCharacter >= stmts.firstStart.column);
    bool beforeEnd = def->endLine < stmts.lastEnd.row ||
                     (def->endLine == stmts.lastEnd.row && def->endCharacter <= stmts.lastEnd.column);
    return afterStart && beforeEnd;
}

/**
 * @brief Validates if a referenced symbol is an outer input parameter or local variable.
 * @param[in] ref Scope reference.
 * @param[in] sc Enclosing scope.
 * @param[in] stmts Selected statement information.
 * @param[out] outInfo Discovered VarInfo.
 * @return True if reference is a valid extracted method input.
 */
bool IsValidMethodInputRef(const analysis::LocalReference& ref, const analysis::Scope* sc,
                           const ExtractMethodStatements& stmts, VarInfo& outInfo)
{
    if (ref.isMemberAccess || ref.startLine < stmts.firstStart.row || ref.endLine > stmts.lastEnd.row)
    {
        return false;
    }
    const analysis::LocalDefinition* def = analysis::ResolveInScope(sc, ref.name);
    if (!def)
    {
        return false;
    }
    if (!ts_node_is_null(stmts.classNode) && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    if (IsDeclaredInsideRange(def, stmts))
    {
        return false;
    }

    std::string tName = def->typeName.empty() ? "auto" : def->typeName;
    outInfo = {def->name, std::move(tName), false};
    return true;
}

/**
 * @brief Finds the enclosing class declaration node for a function node.
 * @param[in] fnNode Function declaration node.
 * @return Class declaration node or null node.
 */
TSNode FindEnclosingClassNode(TSNode fnNode)
{
    TSNode classNode = fnNode;
    while (!ts_node_is_null(classNode) && std::string_view(ts_node_type(classNode)) != "class_declaration")
    {
        classNode = ts_node_parent(classNode);
    }
    return classNode;
}

} // namespace

std::optional<ExtractMethodStatements> FindSelectedStatements(TSNode rootNode, const CodeActionRequest& request)
{
    TSNode fnNode = FindEnclosingFunctionNode(rootNode, request.range);
    if (ts_node_is_null(fnNode))
    {
        return std::nullopt;
    }
    TSNode bodyNode = FindFunctionBodyBlock(fnNode);
    if (ts_node_is_null(bodyNode))
    {
        return std::nullopt;
    }

    std::vector<TSNode> selectedStmts = CollectStatementsInRange(bodyNode, request.range);
    if (selectedStmts.empty())
    {
        return std::nullopt;
    }

    ExtractMethodStatements result;
    result.fnNode = fnNode;
    result.classNode = FindEnclosingClassNode(fnNode);
    result.selectedStmts = std::move(selectedStmts);
    result.startByte = ts_node_start_byte(result.selectedStmts.front());
    result.endByte = ts_node_end_byte(result.selectedStmts.back());
    result.firstStart = ts_node_start_point(result.selectedStmts.front());
    result.lastEnd = ts_node_end_point(result.selectedStmts.back());
    result.selectedCode = std::string(request.sourceCode.substr(result.startByte, result.endByte - result.startByte));
    return result;
}

std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope)
{
    std::vector<VarInfo> inputParams;
    ankerl::unordered_dense::set<std::string> seenInputs;

    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        if (!sc)
        {
            continue;
        }

        for (const auto& ref : sc->references)
        {
            VarInfo info;
            if (IsValidMethodInputRef(ref, sc, stmts, info) && !seenInputs.contains(info.name))
            {
                seenInputs.insert(info.name);
                inputParams.push_back(std::move(info));
            }
        }

        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
    return inputParams;
}

} // namespace angel_lsp::features
