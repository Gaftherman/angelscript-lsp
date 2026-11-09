#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/Utils.h"

#include <algorithm>
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

    const auto hierarchy = GetInheritedTypeHierarchy(sym.containerName, table);
    if (hierarchy.size() <= 1)
    {
        // No bases at all. `override` on such a method is meaningless whatever it names.
        ctx.LogRule("CheckOverride", "as-err-override-no-base", sym);
        ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
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
            if (index.Members(ancestor).methodNames.contains(sym.name))
            {
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        ctx.LogRule("CheckOverride", "as-err-override-no-base", sym);
        ctx.Emit(sym, "as-err-override-no-base", sym.name, sym.containerName);
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
