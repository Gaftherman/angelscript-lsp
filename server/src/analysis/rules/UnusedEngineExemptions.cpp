#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticContext.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/ReflectionCallFacts.h"
#include "analysis/index/RuleIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/UnusedSymbolChecker.h"
#include "analysis/types/TypeSanitization.h"
#include "utils/RegexCache.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <regex>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Compares two parameter types for equality ignoring leading const, trailing decorations, and whitespace.
 * @param[in] actual Actual parameter type from symbol signature.
 * @param[in] expected Expected parameter type from callback configuration.
 * @return True if normalized parameter types are identical, false otherwise.
 */
static bool ParamTypeEquals(std::string_view actual, std::string_view expected)
{
    const std::string_view act = StripTrailingDecorationsView(StripLeadingConst(actual));
    const std::string_view exp = StripTrailingDecorationsView(StripLeadingConst(expected));
    size_t i = 0;
    size_t j = 0;
    while (i < act.size() || j < exp.size())
    {
        while (i < act.size() && std::isspace(static_cast<unsigned char>(act[i])))
        {
            ++i;
        }
        while (j < exp.size() && std::isspace(static_cast<unsigned char>(exp[j])))
        {
            ++j;
        }
        const bool actEnd = (i == act.size());
        const bool expEnd = (j == exp.size());
        if (actEnd || expEnd)
        {
            return actEnd == expEnd;
        }
        if (act[i] != exp[j])
        {
            return false;
        }
        ++i;
        ++j;
    }
    return true;
}

/**
 * @brief Checks if a function's parameter types match any configured engine callback signature.
 * @param[in] sym Candidate function symbol.
 * @param[in] rules Active rules whose engineCallbackSignatures list the expected parameter-type lists.
 * @return True if parameter types match one of the candidate signatures.
 */
bool MatchesEngineCallbackSignature(const Symbol& sym, const config::UnusedSymbolRules& rules)
{
    const auto& signatures = rules.engineCallbackSignatures;
    if (sym.type != SymbolType::Function || signatures.empty() ||
        !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& fn = sym.GetFunction();
    for (const auto& sig : signatures)
    {
        if (fn.parameters.size() != sig.size())
        {
            continue;
        }
        bool match = true;
        for (size_t i = 0; i < sig.size(); ++i)
        {
            if (!ParamTypeEquals(fn.parameters[i].typeName, sig[i]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a class inherits from an engine base class listed in ignoredBaseClasses.
 * @param[in] className Name of the candidate class.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] rules Active unused symbol rules.
 * @return True if class or any ancestor is an engine base class.
 */
bool IsEngineEntity(std::string_view className, const SymbolTable& symbolTable, const config::UnusedSymbolRules& rules)
{
    const auto& ignored = rules.ignoredBaseClasses;
    if (ignored.empty())
    {
        return false;
    }
    if (std::find(ignored.begin(), ignored.end(), className) != ignored.end())
    {
        return true;
    }
    auto hierarchy = GetInheritedTypeHierarchy(className, symbolTable);
    if (const auto ruleIndex = symbolTable.GetRuleIndex())
    {
        auto it = ruleIndex->hostClassesByMixin.find(className);
        if (it != ruleIndex->hostClassesByMixin.end())
        {
            for (const auto& host : it->second)
            {
                const std::string_view hostKey =
                    host.qualifiedName.empty() ? std::string_view(host.name) : std::string_view(host.qualifiedName);
                const auto hostHier = GetInheritedTypeHierarchy(hostKey, symbolTable);
                hierarchy.insert(hierarchy.end(), hostHier.begin(), hostHier.end());
            }
        }
    }
    return std::any_of(hierarchy.begin(), hierarchy.end(), [&ignored](std::string_view b)
                       { return std::find(ignored.begin(), ignored.end(), b) != ignored.end(); });
}

/**
 * @brief Checks if a method matches any lifecycle method configured for a base class.
 * @param[in] className Name of the candidate base class.
 * @param[in] methodName Name of the candidate method.
 * @param[in] unusedRules Active unused symbol rules.
 * @return True if methodName is configured as a lifecycle method for className.
 */
static bool CheckBaseClassLifecycleMethods(std::string_view className, std::string_view methodName,
                                           const config::UnusedSymbolRules& unusedRules)
{
    auto it = unusedRules.baseClassLifecycleMethods.find(className);
    if (it == unusedRules.baseClassLifecycleMethods.end() && utils::ContainsScopeSeparator(className))
    {
        it = unusedRules.baseClassLifecycleMethods.find(LastScopeSegment(className));
    }
    return it != unusedRules.baseClassLifecycleMethods.end() &&
           std::find(it->second.begin(), it->second.end(), methodName) != it->second.end();
}

/**
 * @brief Checks if a method overrides a base class method or matches an engine lifecycle callback.
 * @param[in] sym Candidate member function symbol.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] unusedRules Active unused symbol rules.
 * @return True if method is an override or lifecycle callback.
 */
bool IsLifecycleOrOverride(const Symbol& sym, const SymbolTable& symbolTable,
                           const config::UnusedSymbolRules& unusedRules)
{
    if (sym.containerName.empty())
    {
        return false;
    }

    LinkedBaseOptions options;
    options.delegateProps = unusedRules.baseClassDelegateProperties;
    options.proxyMap = &unusedRules.baseClassProxyMap;

    const auto overridden = FindOverriddenBaseMethodSymbols(sym.containerName, sym.name, symbolTable, options);
    if (!overridden.empty())
    {
        return true;
    }

    auto hierarchy = GetInheritedTypeHierarchy(sym.containerName, symbolTable);
    ExpandMixinHostHierarchies(sym.containerName, symbolTable, hierarchy);

    const bool hasIgnoredBases = !unusedRules.ignoredBaseClasses.empty();
    bool isEntity = !hasIgnoredBases;
    const bool hasBaseMethods = !unusedRules.baseClassLifecycleMethods.empty();
    const bool hasOptions = options.HasActiveOptions();

    for (std::string_view ancestor : hierarchy)
    {
        if (hasIgnoredBases && !isEntity)
        {
            if (std::find(unusedRules.ignoredBaseClasses.begin(), unusedRules.ignoredBaseClasses.end(), ancestor) !=
                unusedRules.ignoredBaseClasses.end())
            {
                isEntity = true;
            }
        }
        if (hasBaseMethods && CheckBaseClassLifecycleMethods(ancestor, sym.name, unusedRules))
        {
            return true;
        }
        if (hasOptions && hasBaseMethods)
        {
            const auto linkedBases = ResolveLinkedBaseClasses(ancestor, symbolTable, options);
            for (std::string_view lb : linkedBases)
            {
                if (CheckBaseClassLifecycleMethods(lb, sym.name, unusedRules))
                {
                    return true;
                }
            }
        }
    }

    if (isEntity)
    {
        const auto& methods = unusedRules.lifecycleMethods;
        if (std::find(methods.begin(), methods.end(), sym.name) != methods.end())
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Checks if a symbol is an engine-managed entry point (global hook or class lifecycle method).
 * @param[in] sym Candidate symbol.
 * @param[in] symbolTable Symbol table providing type inheritance hierarchy.
 * @param[in] unusedRules Active unused symbol rules.
 * @return True if symbol is invoked directly by the engine host.
 */
bool IsEngineManagedFunction(const Symbol& sym, const SymbolTable& symbolTable,
                             const config::UnusedSymbolRules& unusedRules)
{
    if (sym.type != SymbolType::Function)
    {
        return false;
    }

    if (sym.containerName.empty() || sym.containerKind == ContainerKind::Namespace)
    {
        if (std::find(unusedRules.ignoredGlobalFunctions.begin(), unusedRules.ignoredGlobalFunctions.end(), sym.name) !=
                unusedRules.ignoredGlobalFunctions.end() ||
            (!sym.qualifiedName.empty() &&
             std::find(unusedRules.ignoredGlobalFunctions.begin(), unusedRules.ignoredGlobalFunctions.end(),
                       sym.qualifiedName) != unusedRules.ignoredGlobalFunctions.end()))
        {
            return true;
        }
        if (MatchesEngineCallbackSignature(sym, unusedRules))
        {
            return true;
        }
        return utils::MatchesCachedRegex(sym.name, unusedRules.ignoredGlobalFunctionRegex) ||
               (!sym.qualifiedName.empty() &&
                utils::MatchesCachedRegex(sym.qualifiedName, unusedRules.ignoredGlobalFunctionRegex));
    }

    return IsLifecycleOrOverride(sym, symbolTable, unusedRules);
}

/**
 * @brief Checks if a function symbol is explicitly ignored by diagnostic configuration.
 * @param[in] sym Candidate function symbol.
 * @param[in] request Semantic analysis request containing diagnostic settings.
 * @return True if symbol is ignored by diagnostic configuration, false otherwise.
 */
bool IsIgnoredByDiagnosticsConfig(const Symbol& sym, const SemanticAnalysisRequest& request)
{
    if (!request.diagnostics)
    {
        return false;
    }
    const auto& diag = *request.diagnostics;
    if (diag.ignoredUnusedFunctions.contains(sym.name) ||
        (!sym.qualifiedName.empty() && diag.ignoredUnusedFunctions.contains(sym.qualifiedName)))
    {
        return true;
    }
    for (const auto& pattern : diag.ignoredUnusedFunctionPatterns)
    {
        if (utils::MatchesCachedRegex(sym.name, pattern) ||
            (!sym.qualifiedName.empty() && utils::MatchesCachedRegex(sym.qualifiedName, pattern)))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a function symbol is ignored by engine rules or diagnostic configuration.
 * @param[in] sym Candidate function symbol.
 * @param[in] request Semantic analysis request containing diagnostic settings and symbol table.
 * @param[in] rules Active unused symbol rules.
 * @return True if function is ignored by rules or diagnostics, false otherwise.
 */
bool IsIgnoredFunction(const Symbol& sym, const SemanticAnalysisRequest& request,
                       const config::UnusedSymbolRules& rules)
{
    if (IsIgnoredByDiagnosticsConfig(sym, request))
    {
        return true;
    }
    return IsEngineManagedFunction(sym, request.symbolTable, rules);
}

} // namespace angel_lsp::analysis::rules
