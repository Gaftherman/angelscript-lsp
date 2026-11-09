#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ClassRuleFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/InterfaceImplementation.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/ClassRules.h"
#include "document/Document.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Checks if a class declaration is missing a body.
 */
static void CheckBodyPresence(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.hasBraces && !sig.modifiers.isExternal)
    {
        bool hasFullDefinition = false;
        if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
        {
            for (const auto& s : *symsPtr)
            {
                if (s.type == SymbolType::Class && s.GetClass().hasBraces)
                {
                    hasFullDefinition = true;
                    break;
                }
            }
        }
        if (!hasFullDefinition)
        {
            ctx.LogRule("CheckClassModifiers", "as-err-declaration-missing-body", sym);
            ctx.Emit(sym, "as-err-declaration-missing-body", sym.name);
        }
    }
}

/**
 * @brief Validates external and shared rules on a class declaration.
 */
static void CheckExternalClass(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.modifiers.isExternal)
    {
        return;
    }

    if (!sig.modifiers.isShared)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-external-not-shared", sym);
        ctx.Emit(sym, "as-err-external-not-shared", sym.name);
        return;
    }

    const bool knowsModules = ctx.request.moduleContext.has_value() && !ctx.request.moduleContext->name.empty();
    bool hasFullSharedDefinition = false;

    if (knowsModules)
    {
        hasFullSharedDefinition = ctx.request.moduleContext->sharedElsewhere.contains(sym.name);
    }
    else if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(sym.name))
    {
        for (const auto& s : *symsPtr)
        {
            if (s.type == SymbolType::Class && s.GetClass().hasBraces && s.GetClass().modifiers.isShared &&
                !s.GetClass().modifiers.isExternal)
            {
                hasFullSharedDefinition = true;
                break;
            }
        }
    }

    if (!hasFullSharedDefinition)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-external-not-found", sym);
        ctx.Emit(sym, "as-err-external-not-found", sym.name);
    }
}

/**
 * @brief Validates mixin-specific modifier restrictions.
 */
static void CheckMixinModifiers(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.modifiers.isMixin)
    {
        return;
    }

    if (sig.modifiers.isFinal)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-final", sym);
        ctx.Emit(sym, "as-err-mixin-final", sym.name);
    }

    if (sig.modifiers.isAbstract)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-abstract", sym);
        ctx.Emit(sym, "as-err-mixin-abstract", sym.name);
    }

    if (ctx.request.GetRuleIndex().Members(sym.qualifiedName).hasNestedType)
    {
        ctx.LogRule("CheckClassModifiers", "as-err-mixin-child-type", sym);
        ctx.Emit(sym, "as-err-mixin-child-type", sym.name);
    }
}

/**
 * @brief Modifier and shape rules a class declaration must satisfy on its own.
 */
void CheckClassModifiers(const Symbol& sym, const ClassSignature& sig, const DiagnosticContext& ctx)
{
    CheckBodyPresence(sym, sig, ctx);
    CheckExternalClass(sym, sig, ctx);
    CheckMixinModifiers(sym, sig, ctx);
}

/**
 * @brief Collects getter and setter functions declared in the specified container.
 * @param[in] container Qualified or unqualified container name to filter accessors by.
 * @param[in] ctx Diagnostic context providing symbol table access.
 * @param[out] getters Destination map for discovered getter accessors.
 * @param[out] setters Destination map for discovered setter accessors.
 */
static void CollectPropertyAccessors(std::string_view container, const DiagnosticContext& ctx,
                                     ankerl::unordered_dense::map<std::string_view, const Symbol*>& getters,
                                     ankerl::unordered_dense::map<std::string_view, const Symbol*>& setters)
{
    ctx.request.symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] const auto& qName, const std::vector<Symbol>& syms)
        {
            for (const auto& s : syms)
            {
                if (s.containerName == container && s.type == SymbolType::Function &&
                    std::holds_alternative<FunctionSignature>(s.signature))
                {
                    const auto& fn = s.GetFunction();
                    if (fn.modifiers.isProperty)
                    {
                        if (s.name.starts_with("get_") && s.name.size() > 4)
                        {
                            getters[std::string_view(s.name).substr(4)] = &s;
                        }
                        else if (s.name.starts_with("set_") && s.name.size() > 4)
                        {
                            setters[std::string_view(s.name).substr(4)] = &s;
                        }
                    }
                }
            }
        });
}

/**
 * @brief Validates matching types between getter returns and setter parameter.
 * @param[in] getters Map of property names to getter symbols.
 * @param[in] setters Map of property names to setter symbols.
 * @param[in] ctx Diagnostic context receiving mismatch diagnostics.
 */
static void ValidatePropertyAccessorPairs(const ankerl::unordered_dense::map<std::string_view, const Symbol*>& getters,
                                          const ankerl::unordered_dense::map<std::string_view, const Symbol*>& setters,
                                          const DiagnosticContext& ctx)
{
    for (const auto& [propName, getSym] : getters)
    {
        auto it = setters.find(propName);
        if (it != setters.end())
        {
            const Symbol* setSym = it->second;
            const std::string_view getRet = CleanBaseTypeView(getSym->GetFunction().returnType);
            const std::string_view setParam = !setSym->GetFunction().parameters.empty()
                                                  ? CleanBaseTypeView(setSym->GetFunction().parameters.back().typeName)
                                                  : "";
            if (!getRet.empty() && !setParam.empty() && getRet != setParam)
            {
                ctx.LogRule("CheckPropertyAccessors", "as-err-property-type-mismatch", *setSym);
                ctx.Emit(*setSym, "as-err-property-type-mismatch", {propName, getRet, setParam});
            }
        }
    }
}

/**
 * @brief Reports property accessor get/set type mismatches within a class/interface.
 * @param[in] sym Class or interface symbol to analyze.
 * @param[in] ctx Diagnostic context receiving mismatch diagnostics.
 */
void CheckPropertyAccessors(const Symbol& sym, const DiagnosticContext& ctx)
{
    const std::string_view container =
        sym.qualifiedName.empty() ? std::string_view(sym.name) : std::string_view(sym.qualifiedName);
    if (container.empty())
    {
        return;
    }

    ankerl::unordered_dense::map<std::string_view, const Symbol*> getters;
    ankerl::unordered_dense::map<std::string_view, const Symbol*> setters;

    CollectPropertyAccessors(container, ctx, getters, setters);
    ValidatePropertyAccessorPairs(getters, setters, ctx);
}

} // namespace angel_lsp::analysis::rules
