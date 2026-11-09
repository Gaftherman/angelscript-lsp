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
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Emits diagnostics for unused variable or field definitions in a single lexical scope.
 * @param[in] scope Lexical scope whose definitions to inspect.
 * @param[in] isFunc True if the scope is within a function body.
 * @param[in] uCtx Context holding reference and reflection sets.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
static void CheckScopeDefinitions(const Scope* scope, bool isFunc, const UnusedCheckContext& uCtx,
                                  DiagnosticContext& ctx)
{
    for (const auto& def : scope->definitions)
    {
        if (uCtx.rules.checkLocals && isFunc && def.kind == LocalDefinitionKind::Variable &&
            !uCtx.usedLocals.contains(&def))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            diagnostics::codes::UnusedVariable, def.name, DiagnosticSeverity::Warning);
        }
        else if (uCtx.rules.checkMembers && def.kind == LocalDefinitionKind::Field &&
                 !uCtx.refNames.contains(def.name) && !uCtx.reflections.contains(def.name))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            diagnostics::codes::UnusedField, def.name, DiagnosticSeverity::Warning);
        }
    }
}

/**
 * @brief Traverses the scope tree iteratively to emit diagnostics for unused local variables and fields.
 * @param[in] rootScope Root of the lexical scope tree to walk.
 * @param[in] uCtx Context holding reference and reflection sets.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckLocalsAndFields(const Scope* rootScope, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!rootScope)
        return;

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        bool isFunc = false;
        for (const Scope* cur = scope; cur != nullptr && !isFunc; cur = cur->parent)
            isFunc = cur->isFunctionScope;

        CheckScopeDefinitions(scope, isFunc, uCtx, ctx);

        for (const auto& child : scope->children)
        {
            if (child)
                worklist.push_back(child.get());
        }
    }
}

/**
 * @brief Checks if a global variable's type is exempted from unused symbol warnings.
 * @param[in] var Variable signature containing declared and base type names.
 * @param[in] rules Active unused symbol rules specifying exempt global type names.
 * @return True if the variable type matches an exempt type, false otherwise.
 */
[[nodiscard]] static bool IsExemptGlobalType(const VariableSignature& var, const config::UnusedSymbolRules& rules)
{
    if (rules.unusedGlobalExemptTypes.empty())
    {
        return false;
    }
    const std::string_view cleanType = CleanBaseTypeView(var.typeName);
    for (const auto& exempt : rules.unusedGlobalExemptTypes)
    {
        const std::string_view exemptView{exempt};
        if (cleanType == exemptView || var.baseTypeName == exemptView)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a symbol represents a candidate global variable for unused checking.
 * @param[in] sym Symbol under test.
 * @param[in] ignoreCallInit True to ignore variables initialized with a call.
 * @param[in] rules Active unused symbol rules specifying exempt global types.
 * @param[in] req Analysis request containing diagnostic settings.
 * @return True if symbol is a candidate global variable, false otherwise.
 */
[[nodiscard]] static bool IsCandidateGlobalVariable(const Symbol& sym, bool ignoreCallInit,
                                                    const config::UnusedSymbolRules& rules,
                                                    const SemanticAnalysisRequest& req)
{
    if (sym.type != SymbolType::Variable || !sym.containerName.empty() ||
        !std::holds_alternative<VariableSignature>(sym.signature))
    {
        return false;
    }
    const auto& var = sym.GetVariable();
    if (var.isEnumConstant || (ignoreCallInit && var.hasCallInitializer))
    {
        return false;
    }
    return !IsExemptGlobalType(var, rules) && !IsIgnoredByDiagnosticsConfig(sym, req);
}

/**
 * @brief Checks if a symbol is unreferenced across document references and reflection calls.
 * @param[in] name Short symbol name.
 * @param[in] qualifiedName Qualified symbol name.
 * @param[in] uCtx Context holding reference and reflection sets.
 * @return True if symbol is unreferenced, false otherwise.
 */
[[nodiscard]] static bool IsSymbolUnreferenced(std::string_view name, std::string_view qualifiedName,
                                               const UnusedCheckContext& uCtx)
{
    return !uCtx.refNames.contains(name) && !uCtx.reflections.contains(name) &&
           !uCtx.reflections.contains(qualifiedName);
}

/**
 * @brief Emits warnings for unreferenced global variables in the analyzed file.
 * @param[in] req Semantic analysis request containing symbols and configuration.
 * @param[in] uCtx Context holding reference sets and unused checking rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedGlobals(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkGlobals)
        return;

    const bool ignoreCallInit = uCtx.rules.ignoreUnusedGlobalsWithCallInitializer ||
                                (req.diagnostics && req.diagnostics->ignoreUnusedGlobalsWithCallInitializer);

    req.symbolTable.ForEachSymbolInFile(req.fileUri,
                                        [&]([[maybe_unused]] std::string_view qName, const std::vector<Symbol>& syms)
                                        {
                                            for (const auto& sym : syms)
                                            {
                                                if (!IsCandidateGlobalVariable(sym, ignoreCallInit, uCtx.rules, req))
                                                {
                                                    continue;
                                                }
                                                if (IsSymbolUnreferenced(sym.name, sym.qualifiedName, uCtx))
                                                {
                                                    ctx.Emit(sym, diagnostics::codes::UnusedGlobalVariable, sym.name,
                                                             DiagnosticSeverity::Warning);
                                                }
                                            }
                                        });
}

/**
 * @brief Checks whether a property accessor method is referenced by its property name.
 * @param[in] name Method name beginning with get_ or set_.
 * @param[in] refNames Set of referenced symbol names.
 * @return True if the property name following the accessor prefix is referenced, false otherwise.
 */
[[nodiscard]] static bool IsPropertyAccessorUsed(std::string_view name, const NameViewSet& refNames)
{
    if (name.size() <= 4 || (!name.starts_with("get_") && !name.starts_with("set_")))
        return false;
    return refNames.contains(name.substr(4));
}

/**
 * @brief Checks if a method name is exempt from unused warnings (constructors, destructors, operators, accessors).
 * @param[in] name Method name under test.
 * @param[in] containerName Name of the enclosing class.
 * @return True if method name is exempt from unused warnings, false otherwise.
 */
[[nodiscard]] static bool IsExemptMethodName(std::string_view name, std::string_view containerName) noexcept
{
    return name == containerName || name.starts_with('~') || name.starts_with("op") || name.starts_with("get_") ||
           name.starts_with("set_");
}

/**
 * @brief Checks if a class is inherited or mixed into another class.
 * @param[in] className Name of candidate class.
 * @param[in] uCtx Unused check context holding inheritance hierarchy.
 * @return True if class is inherited or mixed in, false otherwise.
 */
[[nodiscard]] static bool IsClassInherited(std::string_view className, const UnusedCheckContext& uCtx)
{
    return uCtx.inheritedOrMixedInClasses.contains(className);
}

/**
 * @brief Checks if a method is overridden in any derived class.
 * @param[in] sym Candidate method symbol.
 * @param[in] uCtx Unused check context holding class hierarchy and rule index.
 * @return True if method is overridden in a derived class, false otherwise.
 */
[[nodiscard]] static bool IsMethodOverriddenInDerived(const Symbol& sym, const UnusedCheckContext& uCtx)
{
    if (sym.containerName.empty())
        return false;
    auto it = uCtx.derivedClassesByBase.find(sym.containerName);
    if (it == uCtx.derivedClassesByBase.end())
        return false;
    for (const auto& derived : it->second)
    {
        if (uCtx.ruleIndex.Members(derived).methodNames.contains(sym.name))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Evaluates whether an unused warning should be skipped for a given function symbol.
 * @param[in] sym Candidate function symbol.
 * @param[in] req Semantic analysis request containing symbols and configuration.
 * @param[in] uCtx Context holding reference sets and unused checking rules.
 * @return True if the function should be skipped, false otherwise.
 */
[[nodiscard]] static bool ShouldSkipFunction(const Symbol& sym, const SemanticAnalysisRequest& req,
                                             const UnusedCheckContext& uCtx)
{
    if (sym.isSynthesized)
        return true;
    if (std::holds_alternative<FunctionSignature>(sym.signature) && sym.GetFunction().modifiers.isAbstract)
        return true;
    if (!sym.containerName.empty())
    {
        if (const auto contSym = req.symbolTable.FindFirstSymbol(sym.containerName))
        {
            if (contSym->type == SymbolType::Interface)
                return true;
        }
    }
    if (IsIgnoredFunction(sym, req, uCtx.rules) || uCtx.reflections.contains(sym.name) ||
        uCtx.reflections.contains(sym.qualifiedName))
        return true;
    if (IsExemptMethodName(sym.name, sym.containerName) || IsPropertyAccessorUsed(sym.name, uCtx.refNames))
        return true;
    if (IsLifecycleOrOverride(sym, req.symbolTable, uCtx.rules) || IsMethodOverriddenInDerived(sym, uCtx))
        return true;
    return false;
}

/**
 * @brief Emits warnings for unreferenced functions and methods in the analyzed file.
 * @param[in] req Semantic analysis request containing symbols and configuration.
 * @param[in] uCtx Context holding reference sets and unused checking rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedFunctions(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkFunctions && !uCtx.rules.checkMembers)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] std::string_view qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type != SymbolType::Function || ShouldSkipFunction(sym, req, uCtx) ||
                    uCtx.refNames.contains(sym.name))
                    continue;

                if (sym.containerName.empty() || sym.containerKind == ContainerKind::Namespace)
                {
                    if (uCtx.rules.checkFunctions)
                    {
                        ctx.Emit(sym, diagnostics::codes::UnusedFunction, sym.name, DiagnosticSeverity::Warning);
                    }
                }
                else
                {
                    if (uCtx.rules.checkFunctions || uCtx.rules.checkMembers)
                    {
                        ctx.Emit(sym, diagnostics::codes::UnusedMethod, sym.name, DiagnosticSeverity::Warning);
                    }
                }
            }
        });
}

/**
 * @brief Emits warnings for unreferenced class definitions in the analyzed file.
 * @param[in] req Semantic analysis request containing symbols and configuration.
 * @param[in] uCtx Context holding reference sets and unused checking rules.
 * @param[in,out] ctx Diagnostic context accumulating emitted warnings.
 */
void CheckUnusedClasses(const SemanticAnalysisRequest& req, const UnusedCheckContext& uCtx, DiagnosticContext& ctx)
{
    if (!uCtx.rules.checkClasses)
        return;

    req.symbolTable.ForEachSymbolInFile(
        req.fileUri,
        [&]([[maybe_unused]] std::string_view qName, const std::vector<Symbol>& syms)
        {
            for (const auto& sym : syms)
            {
                if (sym.type == SymbolType::Class && !IsEngineEntity(sym.name, req.symbolTable, uCtx.rules) &&
                    !IsIgnoredByDiagnosticsConfig(sym, req) && !uCtx.reflections.contains(sym.name) &&
                    !uCtx.reflections.contains(sym.qualifiedName) && !uCtx.refNames.contains(sym.name) &&
                    !IsClassInherited(sym.name, uCtx))
                {
                    ctx.Emit(sym, diagnostics::codes::UnusedClass, sym.name, DiagnosticSeverity::Warning);
                }
            }
        });
}

} // namespace angel_lsp::analysis::rules
