#include "analysis/rules/UnusedSymbolChecker.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ReflectionCallFacts.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/ScopedNameUtils.h"
#include "utils/StringPool.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{

/**
 * @brief Resolves effective unused symbol rules by combining engine rules with diagnostic flags.
 * @param[in] request Semantic analysis request containing diagnostic settings and engine rules.
 * @return Effective unused symbol rules for analysis.
 */
static config::UnusedSymbolRules ResolveEffectiveRules(const SemanticAnalysisRequest& request)
{
    config::UnusedSymbolRules rules =
        request.engineRules ? request.engineRules->unusedRules : config::UnusedSymbolRules{};
    if (!rules.enabled)
    {
        rules.checkMembers = request.diagnostics && request.diagnostics->reportUnusedMembers;
        rules.checkGlobals = request.diagnostics && request.diagnostics->reportUnusedGlobalVariables;
        rules.checkFunctions = request.diagnostics && request.diagnostics->reportUnusedFunctions;
        rules.checkClasses = request.diagnostics && request.diagnostics->reportUnusedClasses;
        return rules;
    }
    if (request.diagnostics)
    {
        const auto& diag = *request.diagnostics;
        if (diag.reportUnusedGlobalVariablesExplicit)
        {
            rules.checkGlobals = diag.reportUnusedGlobalVariables;
        }
        if (diag.reportUnusedFunctionsExplicit)
        {
            rules.checkFunctions = diag.reportUnusedFunctions;
        }
        if (diag.reportUnusedClassesExplicit)
        {
            rules.checkClasses = diag.reportUnusedClasses;
        }
        if (diag.reportUnusedMembersExplicit)
        {
            rules.checkMembers = diag.reportUnusedMembers;
        }
    }
    return rules;
}

/**
 * @brief Populates sets and maps of inherited and derived class names from the rule index.
 * @param[in] ruleIndex Pre-indexed class inheritance and mixin relations.
 * @param[out] inheritedOrMixedInClasses Set receiving names of base classes and mixins.
 * @param[out] derivedClassesByBase Map receiving derived class names grouped by base name.
 */
static void BuildInheritedAndDerivedMaps(
    const RuleIndex& ruleIndex, NameViewSet& inheritedOrMixedInClasses,
    ankerl::unordered_dense::map<std::string_view, std::vector<std::string_view>>& derivedClassesByBase)
{
    for (const auto& [base, derivedList] : ruleIndex.derivedByBase)
    {
        const std::string_view baseView = base;
        inheritedOrMixedInClasses.insert(baseView);
        auto& targets = derivedClassesByBase[baseView];
        for (const auto& d : derivedList)
        {
            targets.push_back(d.qualifiedName.empty() ? std::string_view{d.name} : std::string_view{d.qualifiedName});
            if (!d.name.empty() && d.name != d.qualifiedName)
            {
                targets.push_back(std::string_view{d.name});
            }
        }
    }
    for (const auto& [mixin, hostList] : ruleIndex.hostClassesByMixin)
    {
        const std::string_view mixinView = mixin;
        inheritedOrMixedInClasses.insert(mixinView);
        auto& targets = derivedClassesByBase[mixinView];
        for (const auto& h : hostList)
        {
            targets.push_back(h.qualifiedName.empty() ? std::string_view{h.name} : std::string_view{h.qualifiedName});
            if (!h.name.empty() && h.name != h.qualifiedName)
            {
                targets.push_back(std::string_view{h.name});
            }
        }
    }
}

/**
 * @brief Checks for unused local variables, member variables, global variables, functions, and classes.
 * @param[in] request Semantic analysis request containing AST, symbol table, and engine rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedSymbols(const SemanticAnalysisRequest& request, DiagnosticContext& ctx)
{
    if (!request.scopeRoot || utils::IsPredefinedFile(request.fileUri, request.predefinedFileExtension))
    {
        return;
    }

    config::UnusedSymbolRules rules = ResolveEffectiveRules(request);
    utils::StringPool pool;

    ankerl::unordered_dense::set<uint64_t> defLocs;
    CollectDefLocations(request.scopeRoot.get(), defLocs);

    const bool checkAnyCrossFile = rules.checkGlobals || rules.checkFunctions || rules.checkClasses;

    NameViewSet refNames;
    CollectDocReferences(request.scopeRoot.get(), DocRefCollectContext{defLocs, request.sourceCode, &rules}, refNames,
                         pool);
    if (checkAnyCrossFile)
    {
        CollectCrossFileReferences(request, refNames, pool);
    }

    ankerl::unordered_dense::set<const LocalDefinition*> usedLocals;
    CollectUsedLocals(request.scopeRoot.get(), usedLocals);

    const auto& ruleIndex = request.GetRuleIndex();

    NameViewSet inheritedOrMixedInClasses;
    ankerl::unordered_dense::map<std::string_view, std::vector<std::string_view>> derivedClassesByBase;
    BuildInheritedAndDerivedMaps(ruleIndex, inheritedOrMixedInClasses, derivedClassesByBase);

    const NameViewSet reflections = checkAnyCrossFile ? CollectStringReflection(request, pool) : NameViewSet{};
    const UnusedCheckContext uCtx{
        refNames, usedLocals, reflections, rules, inheritedOrMixedInClasses, derivedClassesByBase, ruleIndex};

    CheckLocalsAndFields(request.scopeRoot.get(), uCtx, ctx);
    CheckUnusedGlobals(request, uCtx, ctx);
    CheckUnusedFunctions(request, uCtx, ctx);
    CheckUnusedClasses(request, uCtx, ctx);
}

} // namespace angel_lsp::analysis::rules
