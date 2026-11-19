#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
void CheckConstructorDestructor(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                                const DiagnosticContext& ctx)
{
    if (fctx.isMixin && fctx.isConstructor && !sym.isSynthesized)
    {
        ctx.LogRule("CheckConstructorDestructor", diagnostics::codes::MixinConstructor, sym);
        ctx.Emit(sym, diagnostics::codes::MixinConstructor, sym.containerName);
    }

    if (fctx.isConstructor && !fctx.isInterface && !sym.isSynthesized && !sig.returnType.empty() && sig.hasBody)
    {
        ctx.LogRule("CheckConstructorDestructor", diagnostics::codes::MethodNamedWithClassName, sym);
        ctx.Emit(sym, diagnostics::codes::MethodNamedWithClassName, sym.name);
    }

    if (!fctx.isDestructor)
    {
        return;
    }

    if (fctx.isMixin)
    {
        ctx.LogRule("CheckConstructorDestructor", diagnostics::codes::MixinDestructor, sym);
        ctx.Emit(sym, diagnostics::codes::MixinDestructor, sym.containerName);
    }

    if (!sig.parameters.empty())
    {
        ctx.LogRule("CheckConstructorDestructor", diagnostics::codes::DestructorParam, sym);
        ctx.Emit(sym, diagnostics::codes::DestructorParam, sym.name);
    }

    if (!sig.returnType.empty())
    {
        ctx.LogRule("CheckConstructorDestructor", diagnostics::codes::DestructorReturnType, sym);
        ctx.Emit(sym, diagnostics::codes::DestructorReturnType, sym.name);
    }

    if (sig.modifiers.isDelete)
    {
        ctx.LogRule("CheckConstructorDestructor", "as-err-destructor-delete", sym);
        ctx.Emit(sym, "as-err-destructor-delete", sym.name);
    }
}

// Override

static const Symbol& ResolveOverrideDiagnosticTarget(const Symbol& methodSym, const SymbolTable& table,
                                                     std::shared_ptr<const std::vector<Symbol>>& keepAlive)
{
    if (!methodSym.containerName.empty())
    {
        keepAlive = table.FindSymbolsPtr(methodSym.containerName);
        if (keepAlive)
        {
            const Symbol* best = nullptr;
            for (const auto& candidate : *keepAlive)
            {
                if (candidate.type == SymbolType::Class && candidate.fileUri == methodSym.fileUri &&
                    !candidate.isSynthesized)
                {
                    if (candidate.startLine <= methodSym.startLine && (!best || candidate.startLine > best->startLine))
                    {
                        best = &candidate;
                    }
                    else if (!best)
                    {
                        best = &candidate;
                    }
                }
            }
            if (best)
            {
                return *best;
            }
        }
    }
    return methodSym;
}

static ParameterModifier EffectiveOverrideModifier(const ParameterInformation& param) noexcept
{
    return (param.isReference && param.modifier == ParameterModifier::None) ? ParameterModifier::InOut : param.modifier;
}

static std::string CanonicalOverrideType(std::string_view rawType, const SymbolTable& table)
{
    const std::string_view cleaned = CleanBaseTypeView(rawType);
    if (cleaned.empty() || IsVariableTypeToken(cleaned) || parser::primitives::IsAutoType(cleaned))
    {
        return {};
    }
    const std::string unwrapped = UnwrapTypedef(cleaned, table);
    return std::string(CanonicalizeType(utils::LastScopeSegment(unwrapped)));
}

static bool MatchesOverrideFunctionSignature(const FunctionSignature& derived, const Symbol& baseSym,
                                             const FunctionSignature& base, const SymbolTable& table)
{
    if (derived.modifiers.isConst != base.modifiers.isConst || derived.parameters.size() != base.parameters.size())
    {
        return false;
    }

    for (size_t i = 0; i < derived.parameters.size(); ++i)
    {
        const auto& dp = derived.parameters[i];
        const auto& bp = base.parameters[i];
        if (EffectiveOverrideModifier(dp) != EffectiveOverrideModifier(bp) || dp.isReference != bp.isReference ||
            dp.isHandle != bp.isHandle || dp.isConst != bp.isConst)
        {
            return false;
        }

        const std::string_view bRaw = bp.baseTypeName.empty() ? std::string_view(bp.typeName) : bp.baseTypeName;
        if (IsTemplateParameterOfContainer(CleanBaseTypeView(bRaw), baseSym, table))
        {
            continue;
        }

        const std::string dType = CanonicalOverrideType(dp.baseTypeName.empty() ? dp.typeName : dp.baseTypeName, table);
        const std::string bType = CanonicalOverrideType(bRaw, table);
        if (!dType.empty() && !bType.empty() && dType != bType)
        {
            return false;
        }
    }
    return true;
}

static bool HasMatchingBaseOverride(std::string_view ancestor, const Symbol& sym, const FunctionSignature& sig,
                                    const SymbolTable& table)
{
    if (const auto candidates = table.FindMemberSymbolPtr(ancestor, sym.name))
    {
        for (const auto& cand : *candidates)
        {
            if (cand.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(cand.signature) &&
                MatchesOverrideFunctionSignature(sig, cand, cand.GetFunction(), table))
            {
                return true;
            }
        }
    }

    if (sym.name.size() <= 4)
    {
        return false;
    }
    const bool isGet = sym.name.starts_with("get_");
    const bool isSet = sym.name.starts_with("set_");
    if (!isGet && !isSet)
    {
        return false;
    }

    const auto props = table.FindMemberSymbolPtr(ancestor, std::string_view(sym.name).substr(4));
    if (!props)
    {
        return false;
    }
    for (const auto& prop : *props)
    {
        if (prop.type != SymbolType::Property || !std::holds_alternative<VariableSignature>(prop.signature))
        {
            continue;
        }
        const auto& var = prop.GetVariable();
        if (!var.isVirtualProperty)
        {
            continue;
        }
        if (isGet && var.hasGet && var.isGetConst == sig.modifiers.isConst && sig.parameters.size() <= 1)
        {
            return true;
        }
        if (isSet && var.hasSet && !sig.modifiers.isConst && !sig.parameters.empty() && sig.parameters.size() <= 2)
        {
            const auto& valParam = sig.parameters.back();
            const std::string dType =
                CanonicalOverrideType(valParam.baseTypeName.empty() ? valParam.typeName : valParam.baseTypeName, table);
            const std::string bType =
                CanonicalOverrideType(var.baseTypeName.empty() ? var.typeName : var.baseTypeName, table);
            if (dType.empty() || bType.empty() || dType == bType)
            {
                return true;
            }
        }
    }
    return false;
}

void CheckOverride(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                   const DiagnosticContext& ctx)
{
    if (!sig.modifiers.isOverride || !fctx.isMember || fctx.isDestructor)
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;

    // The whole point of the rule is that no base declares the method, so it may only speak
    // when every base is visible. One unresolved link - a class from a file outside the
    // workspace, or a host type - and the answer is unknowable rather than "no".
    if (!HierarchyIsFullyVisible(sym.containerName, table))
    {
        return;
    }

    std::shared_ptr<const std::vector<Symbol>> keepAlive;
    const Symbol& target = ResolveOverrideDiagnosticTarget(sym, table, keepAlive);

    const auto hierarchy = GetInheritedTypeHierarchy(sym.containerName, table);
    if (hierarchy.size() <= 1)
    {
        // No bases at all. `override` on such a method is meaningless whatever it names.
        ctx.LogRule("CheckOverride", "as-err-override-no-base", target);
        ctx.Emit(target, "as-err-override-no-base", sym.name, sym.containerName);
        return;
    }

    bool found = false;
    const RuleIndex& index = ctx.request.GetRuleIndex();
    for (const auto& ancestor : hierarchy)
    {
        if (ancestor != sym.containerName)
        {
            if (index.Members(ancestor).finalMethodNames.contains(sym.name))
            {
                // Handled by CheckFinalOverrides in ClassRules
                return;
            }
            if (HasMatchingBaseOverride(ancestor, sym, sig, table))
            {
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        ctx.LogRule("CheckOverride", "as-err-override-no-base", target);
        ctx.Emit(target, "as-err-override-no-base", sym.name, sym.containerName);
    }
}

static void CheckImportedFunction(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    if (ctx.request.moduleContext.has_value() && !sig.originModule.empty())
    {
        const auto& names = ctx.request.moduleContext->moduleNames;
        if (!names.empty() && std::find(names.begin(), names.end(), sig.originModule) == names.end())
        {
            ctx.LogRule("CheckExternal", "as-hint-import-unknown-module", sym);
            ctx.Emit(sym, "as-hint-import-unknown-module", sig.originModule, DiagnosticSeverity::Hint);
        }
    }
}

/**
 * @brief Checks if a shared function has a complete implementation body in another module or the symbol table.
 * @param[in] name Qualified or unqualified function name to look up.
 * @param[in] ctx Diagnostic context containing module and symbol information.
 * @return True if a full shared definition exists; otherwise false.
 */
static bool HasFullSharedFunctionDefinition(std::string_view name, const DiagnosticContext& ctx)
{
    const bool knowsModules = ctx.request.moduleContext.has_value() && !ctx.request.moduleContext->name.empty();
    if (knowsModules)
    {
        return ctx.request.moduleContext->sharedElsewhere.contains(name);
    }

    if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& s : *symsPtr)
        {
            if (s.type == SymbolType::Function && s.GetFunction().hasBody && s.GetFunction().modifiers.isShared &&
                !s.GetFunction().modifiers.isExternal)
            {
                return true;
            }
        }
    }
    return false;
}

void CheckExternal(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.isImported)
    {
        CheckImportedFunction(sym, sig, ctx);
        return;
    }

    if (!sig.modifiers.isExternal)
    {
        return;
    }

    if (!sig.modifiers.isShared)
    {
        ctx.LogRule("CheckExternal", "as-err-external-not-shared", sym);
        ctx.Emit(sym, "as-err-external-not-shared", sym.name);
        return;
    }

    if (!HasFullSharedFunctionDefinition(sym.name, ctx))
    {
        ctx.LogRule("CheckExternal", "as-err-external-not-found", sym);
        ctx.Emit(sym, "as-err-external-not-found", sym.name);
    }
}

} // namespace angel_lsp::analysis::rules
