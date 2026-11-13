/**
 * @file CodeActionExtractMethodOutputs.cpp
 * @brief Analysis for identifying mutated and output variables in extracted method selection.
 */

#include "analysis/code_action/CodeActionExtractMethodAnalysis.h"
#include "utils/TransparentHash.h"
#include "utils/Utils.h"

namespace angel_lsp::analysis::code_action
{

static bool IsVariableReferencedAfter(const analysis::Scope* fnScope, std::string_view name, uint32_t line,
                                      uint32_t col = 0)
{
    if (!fnScope)
    {
        return false;
    }
    std::vector<const analysis::Scope*> worklist;
    worklist.push_back(fnScope);
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
                if (r.startLine > line || (r.startLine == line && r.startCharacter >= col))
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

static const analysis::LocalDefinition* FindDefinitionInScopeTree(const analysis::Scope* root, std::string_view name)
{
    if (!root)
    {
        return nullptr;
    }
    std::vector<const analysis::Scope*> worklist;
    worklist.push_back(root);
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

static const analysis::LocalDefinition* FindVariableDefinition(const MethodOutputContext& ctx, std::string_view name)
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

static bool IsValidMutatedOutput(const MethodOutputContext& ctx, const analysis::LocalDefinition* def)
{
    if (!def)
    {
        return false;
    }
    if (ctx.stmts.selection.inClass && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    bool declaredBefore = (def->endLine < ctx.stmts.selection.statementsRange.startLine ||
                           (def->endLine == ctx.stmts.selection.statementsRange.startLine &&
                            def->endCharacter <= ctx.stmts.selection.statementsRange.startCharacter) ||
                           def->kind == analysis::LocalDefinitionKind::Parameter);
    return declaredBefore &&
           IsVariableReferencedAfter(ctx.fnScope, def->name, ctx.stmts.selection.statementsRange.endLine,
                                     ctx.stmts.selection.statementsRange.endCharacter);
}

static void CollectMutatedOutputs(const MethodOutputContext& ctx, const TransparentStringSet& mutatedVars,
                                  TransparentStringSet& seenOutputs, std::vector<VarInfo>& outputVars)
{
    for (const auto& mName : mutatedVars)
    {
        const auto* def = FindVariableDefinition(ctx, mName);
        if (IsValidMutatedOutput(ctx, def) && !seenOutputs.contains(def->name))
        {
            seenOutputs.insert(def->name);
            std::string tName = def->typeName.empty() ? "auto" : def->typeName;
            outputVars.emplace_back(def->name, std::move(tName), false);
        }
    }
}

static void CollectInternalDefinitionsUsedAfter(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                                TransparentStringSet& seenOutputs, std::vector<VarInfo>& outputVars)
{
    if (!fnScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> worklist;
    worklist.push_back(fnScope);
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
            if (def.startLine >= stmts.selection.statementsRange.startLine &&
                def.endLine <= stmts.selection.statementsRange.endLine)
            {
                if (IsVariableReferencedAfter(fnScope, def.name, stmts.selection.statementsRange.endLine + 1, 0) &&
                    !seenOutputs.contains(def.name))
                {
                    seenOutputs.insert(def.name);
                    std::string tName = def.typeName.empty() ? "auto" : def.typeName;
                    outputVars.emplace_back(def.name, std::move(tName), true);
                }
            }
        }
        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
}

TransparentStringSet CollectMutatedVariables(const ExtractMethodStatements& stmts, std::string_view sourceCode)
{
    TransparentStringSet mutatedVars;
    for (const auto& r : stmts.selection.mutatedNames)
    {
        mutatedVars.insert(std::string(utils::SliceSource(sourceCode, r.startByte, r.endByte)));
    }
    return mutatedVars;
}

std::vector<VarInfo> CollectMethodOutputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope,
                                          const TransparentStringSet& mutatedVars, const analysis::Scope* stmtScope)
{
    std::vector<VarInfo> outputVars;
    TransparentStringSet seenOutputs;

    if (fnScope)
    {
        MethodOutputContext ctx{stmts, fnScope, stmtScope};
        CollectMutatedOutputs(ctx, mutatedVars, seenOutputs, outputVars);
        CollectInternalDefinitionsUsedAfter(stmts, fnScope, seenOutputs, outputVars);
    }

    return outputVars;
}

} // namespace angel_lsp::analysis::code_action
