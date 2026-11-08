/**
 * @file CodeActionClassMutation.cpp
 * @brief Analysis of method bodies for mutations against class state.
 */

#include "features/code_action/CodeActionClassMutation.h"

#include "features/code_action/CodeActionInternal.h"
#include "utils/Utils.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace angel_lsp::features
{

/**
 * @brief Collects declared class field names from the symbol table and pre-indexed mutation facts.
 * @param[in] context Mutation check context.
 * @return Set of member field names for the active class.
 */
static ankerl::unordered_dense::set<std::string> CollectClassFields(const ClassMutationContext& context)
{
    ankerl::unordered_dense::set<std::string> classFields;
    if (const auto ruleIndex = context.table.GetRuleIndex())
    {
        const auto& cm = ruleIndex->Members(context.className);
        for (const auto& memberKey : cm.memberKeys)
        {
            auto symsPtr = context.table.FindSymbolsPtr(memberKey);
            if (!symsPtr)
            {
                continue;
            }
            for (const auto& sym : *symsPtr)
            {
                if (sym.containerName == context.className &&
                    (sym.type == analysis::SymbolType::Variable || sym.type == analysis::SymbolType::Property))
                {
                    classFields.insert(sym.name);
                }
            }
        }
    }

    for (const auto& r : context.facts.classFieldNames)
    {
        classFields.insert(std::string(utils::SliceSource(context.sourceCode, r.startByte, r.endByte)));
    }
    return classFields;
}

/**
 * @brief Checks if a resolved variable refers to a class field rather than a local variable.
 * @param[in] context Mutation check context.
 * @param[in] classFields Set of known class field names.
 * @param[in] line 0-based source line where the variable is referenced.
 * @param[in] varName Variable name to resolve.
 * @return True if target refers to a class field or outer variable.
 */
static bool IsFieldOrOuterVar(const ClassMutationContext& context,
                              const ankerl::unordered_dense::set<std::string>& classFields, uint32_t line,
                              std::string_view varName)
{
    const analysis::Scope* inner = FindScopeByLineOrRoot(context.scope, line);
    const analysis::LocalDefinition* localDef = analysis::ResolveInScope(inner, varName);

    bool isField = classFields.contains(std::string(varName));
    if (localDef)
    {
        if (localDef->kind == analysis::LocalDefinitionKind::Field)
        {
            isField = true;
        }
        else if (localDef->kind == analysis::LocalDefinitionKind::Variable ||
                 localDef->kind == analysis::LocalDefinitionKind::Parameter)
        {
            if (localDef->startLine >= context.facts.bodyStartLine && localDef->endLine <= context.facts.bodyEndLine)
            {
                isField = false;
            }
        }
    }
    return isField;
}

/**
 * @brief Evaluates whether a single candidate mutation site mutates class state.
 * @param[in] context Mutation check context.
 * @param[in] classFields Set of known class field names.
 * @param[in] site Pre-indexed candidate mutation site.
 * @return True if the site mutates class fields or calls a non-const method on this.
 */
static bool SiteMutatesClassState(const ClassMutationContext& context,
                                  const ankerl::unordered_dense::set<std::string>& classFields,
                                  const analysis::IndexedMutationSite& site)
{
    const std::string_view name = utils::SliceSource(context.sourceCode, site.name.startByte, site.name.endByte);
    switch (site.kind)
    {
    case analysis::MutationSiteKind::NamedTarget:
        return IsFieldOrOuterVar(context, classFields, site.line, name);
    case analysis::MutationSiteKind::ThisMember:
        return classFields.contains(std::string(name));
    case analysis::MutationSiteKind::ThisCall:
    {
        if (name.empty())
        {
            return false;
        }

        auto symsPtr = context.table.FindMemberSymbolPtr(context.className, name);
        if (!symsPtr)
        {
            return false;
        }
        for (const auto& sym : *symsPtr)
        {
            if (sym.type == analysis::SymbolType::Function && sym.containerName == context.className &&
                sym.name == name && !sym.GetFunction().modifiers.isConst)
            {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

/**
 * @brief Checks if a method body mutates class fields or calls non-const methods on `this`.
 * @param[in] context Mutation evaluation context.
 * @return True if method body mutates class state.
 */
bool MethodBodyMutatesClassState(const ClassMutationContext& context)
{
    auto classFields = CollectClassFields(context);
    for (const auto& site : context.facts.sites)
    {
        if (SiteMutatesClassState(context, classFields, site))
        {
            return true;
        }
    }
    return false;
}

} // namespace angel_lsp::features
