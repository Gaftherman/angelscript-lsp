/**
 * @file CodeActionExtractMethodOutputs.cpp
 * @brief Analysis for identifying mutated and output variables in extracted method selection.
 */

#include "features/code_action/CodeActionExtractMethodAnalysis.h"

namespace angel_lsp::features
{
namespace
{

void CheckAssignmentMutation(TSNode curr, std::string_view sourceCode,
                             ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode left = parser::GetChildByField(curr, parser::fields::Left);
    if (ts_node_is_null(left))
    {
        return;
    }
    std::string_view lType = ts_node_type(left);
    if (lType == "identifier" || lType == "scoped_identifier")
    {
        if (std::string varName = GetNodeText(left, sourceCode); !varName.empty())
        {
            mutatedVars.insert(std::move(varName));
        }
    }
}

void CheckIncDecMutation(TSNode curr, std::string_view sourceCode,
                         ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode opNode = parser::GetChildByField(curr, parser::fields::Operator);
    std::string op = GetNodeText(opNode, sourceCode);
    if (op != "++" && op != "--")
    {
        return;
    }
    TSNode arg = parser::GetChildByField(curr, parser::fields::Operand);
    if (ts_node_is_null(arg))
    {
        return;
    }
    std::string_view aType = ts_node_type(arg);
    if (aType == "identifier" || aType == "scoped_identifier")
    {
        if (std::string varName = GetNodeText(arg, sourceCode); !varName.empty())
        {
            mutatedVars.insert(std::move(varName));
        }
    }
}

void CheckCallMutation(TSNode curr, std::string_view sourceCode, ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    TSNode argsNode = parser::GetChildByField(curr, parser::fields::Arguments);
    if (ts_node_is_null(argsNode))
    {
        return;
    }
    uint32_t argCnt = ts_node_named_child_count(argsNode);
    for (uint32_t i = 0; i < argCnt; ++i)
    {
        TSNode arg = ts_node_named_child(argsNode, i);
        std::string argText = GetNodeText(arg, sourceCode);
        if (argText.starts_with("&out ") || argText.starts_with("&inout ") || argText.starts_with("out ") ||
            argText.starts_with("inout "))
        {
            TSNode idNode = parser::GetChildByField(arg, parser::fields::Name);
            if (ts_node_is_null(idNode) && ts_node_named_child_count(arg) > 0)
            {
                idNode = ts_node_named_child(arg, ts_node_named_child_count(arg) - 1);
            }
            if (!ts_node_is_null(idNode))
            {
                if (std::string varName = GetNodeText(idNode, sourceCode); !varName.empty())
                {
                    mutatedVars.insert(std::move(varName));
                }
            }
        }
    }
}

void CheckNodeMutations(TSNode curr, std::string_view sourceCode,
                        ankerl::unordered_dense::set<std::string>& mutatedVars)
{
    std::string_view type = ts_node_type(curr);
    if (type == "assignment_expression")
    {
        CheckAssignmentMutation(curr, sourceCode, mutatedVars);
    }
    else if (type == "postfix_expression" || type == "unary_expression")
    {
        CheckIncDecMutation(curr, sourceCode, mutatedVars);
    }
    else if (type == "call_expression")
    {
        CheckCallMutation(curr, sourceCode, mutatedVars);
    }
}

bool IsVariableUsedAfter(const analysis::Scope* fnScope, const std::string& name, TSPoint lastEnd)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& r : s->references)
        {
            if (!r.isMemberAccess && r.name == name)
            {
                if (r.startLine > lastEnd.row || (r.startLine == lastEnd.row && r.startCharacter >= lastEnd.column))
                {
                    return true;
                }
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return false;
}

const analysis::LocalDefinition* FindDefinitionInScopeTree(const analysis::Scope* root, const std::string& name)
{
    std::vector<const analysis::Scope*> worklist = {root};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& d : s->definitions)
        {
            if (d.name == name)
            {
                return &d;
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return nullptr;
}

struct MethodOutputContext
{
    const ExtractMethodStatements& stmts;
    const analysis::Scope* fnScope;
    const analysis::Scope* stmtScope;
};

const analysis::LocalDefinition* FindVariableDefinition(const MethodOutputContext& ctx, const std::string& name)
{
    if (ctx.stmtScope)
    {
        if (const auto* def = analysis::ResolveInScope(ctx.stmtScope, name))
        {
            return def;
        }
    }
    if (ctx.fnScope)
    {
        if (const auto* def = analysis::ResolveInScope(ctx.fnScope, name))
        {
            return def;
        }
        return FindDefinitionInScopeTree(ctx.fnScope, name);
    }
    return nullptr;
}

bool IsValidMutatedOutput(const MethodOutputContext& ctx, const analysis::LocalDefinition* def)
{
    if (!def)
    {
        return false;
    }
    if (!ts_node_is_null(ctx.stmts.classNode) && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    bool declaredBefore =
        (def->endLine < ctx.stmts.firstStart.row ||
         (def->endLine == ctx.stmts.firstStart.row && def->endCharacter <= ctx.stmts.firstStart.column) ||
         def->kind == analysis::LocalDefinitionKind::Parameter);
    return declaredBefore && IsVariableUsedAfter(ctx.fnScope, def->name, ctx.stmts.lastEnd);
}

void CollectMutatedOutputs(const MethodOutputContext& ctx, const ankerl::unordered_dense::set<std::string>& mutatedVars,
                           ankerl::unordered_dense::set<std::string>& seenOutputs, std::vector<VarInfo>& outputVars)
{
    for (const auto& mName : mutatedVars)
    {
        const auto* def = FindVariableDefinition(ctx, mName);
        if (IsValidMutatedOutput(ctx, def) && !seenOutputs.contains(def->name))
        {
            seenOutputs.insert(def->name);
            std::string tName = def->typeName.empty() ? "auto" : def->typeName;
            outputVars.push_back({def->name, std::move(tName), false});
        }
    }
}

bool IsVariableUsedAfterLine(const analysis::Scope* fnScope, const std::string& name, uint32_t line)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* s = worklist.back();
        worklist.pop_back();
        if (!s)
        {
            continue;
        }
        for (const auto& r : s->references)
        {
            if (!r.isMemberAccess && r.name == name && r.startLine > line)
            {
                return true;
            }
        }
        for (const auto& c : s->children)
        {
            worklist.push_back(c.get());
        }
    }
    return false;
}

void CollectInternalDefinitionsUsedAfter(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                         ankerl::unordered_dense::set<std::string>& seenOutputs,
                                         std::vector<VarInfo>& outputVars)
{
    std::vector<const analysis::Scope*> worklist = {fnScope};
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        if (!sc)
        {
            continue;
        }
        for (const auto& def : sc->definitions)
        {
            if (def.startLine >= stmts.firstStart.row && def.endLine <= stmts.lastEnd.row)
            {
                if (IsVariableUsedAfterLine(fnScope, def.name, stmts.lastEnd.row) && !seenOutputs.contains(def.name))
                {
                    seenOutputs.insert(def.name);
                    std::string tName = def.typeName.empty() ? "auto" : def.typeName;
                    outputVars.push_back({def.name, std::move(tName), true});
                }
            }
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

} // namespace

ankerl::unordered_dense::set<std::string> CollectMutatedVariables(const std::vector<TSNode>& stmts,
                                                                  std::string_view sourceCode)
{
    ankerl::unordered_dense::set<std::string> mutatedVars;
    for (const auto& stmt : stmts)
    {
        if (ts_node_is_null(stmt))
        {
            continue;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(stmt);
        bool visiting = true;
        while (visiting)
        {
            TSNode curr = ts_tree_cursor_current_node(&cursor);
            CheckNodeMutations(curr, sourceCode, mutatedVars);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                continue;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                continue;
            }
            bool backtracked = false;
            while (ts_tree_cursor_goto_parent(&cursor))
            {
                if (ts_tree_cursor_current_node(&cursor).id == stmt.id)
                {
                    break;
                }
                if (ts_tree_cursor_goto_next_sibling(&cursor))
                {
                    backtracked = true;
                    break;
                }
            }
            if (!backtracked)
            {
                visiting = false;
            }
        }
        ts_tree_cursor_delete(&cursor);
    }
    return mutatedVars;
}

std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const ankerl::unordered_dense::set<std::string>& mutatedVars,
                                          const analysis::Scope* stmtScope)
{
    std::vector<VarInfo> outputVars;
    ankerl::unordered_dense::set<std::string> seenOutputs;

    if (fnScope)
    {
        MethodOutputContext ctx{stmts, fnScope, stmtScope};
        CollectMutatedOutputs(ctx, mutatedVars, seenOutputs, outputVars);
        CollectInternalDefinitionsUsedAfter(stmts, fnScope, seenOutputs, outputVars);
    }

    return outputVars;
}

} // namespace angel_lsp::features
