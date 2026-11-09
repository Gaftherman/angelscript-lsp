#include "analysis/rules/VariableRules.h"

#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Rules about the modifiers a declaration carries where it sits.
 */
static void CheckPlacement(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.isEnumConstant || sym.containerKind == ContainerKind::Enum)
    {
        return;
    }

    const bool isClassMember = IsClassOrInterfaceMember(sym, ctx.request.symbolTable);

    // private/protected mean nothing outside a class body.
    if (!isClassMember && sig.modifiers.access != AccessModifier::Public)
    {
        ctx.LogRule("CheckPlacement", "as-err-global-variable-access-modifier", sym);
        ctx.Emit(sym, "as-err-global-variable-access-modifier", sym.name);
    }

    // A host that gives each script its own isolated state builds its engine with
    // asEP_DISALLOW_GLOBAL_VARS, and then a global is a compile error rather than a style
    // question. Off by default, because the engine's default is off.
    if (!isClassMember && ctx.request.DisallowsGlobalVars())
    {
        ctx.LogRule("CheckPlacement", "as-err-global-vars-disallowed", sym);
        ctx.Emit(sym, "as-err-global-vars-disallowed", sym.name);
    }

    // A class property has no initialization phase in which a const could be given its
    // value, so AngelScript does not allow one.
    if (isClassMember && sig.modifiers.isConst && !sig.isVirtualProperty && !sig.modifiers.isHandle)
    {
        ctx.LogRule("CheckPlacement", "as-err-class-member-const", sym);
        ctx.Emit(sym, "as-err-class-member-const", sym.name);
    }
}

/**
 * @brief Rules for a virtual property's accessors.
 */
static void CheckVirtualProperty(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (!sig.isVirtualProperty)
    {
        return;
    }

    // Two `get` blocks in one property is something the engine rejects outright, and the
    // collector has always worked it out - nothing read the answer until now. Independent
    // of the container, so it is checked before the mixin and interface exemptions.
    if (sig.hasDuplicateGet)
    {
        ctx.LogRule("CheckVirtualProperty", "as-err-property-duplicate-accessor", sym);
        ctx.Emit(sym, "as-err-property-duplicate-accessor", sym.name, "get");
    }
    if (sig.hasDuplicateSet)
    {
        ctx.LogRule("CheckVirtualProperty", "as-err-property-duplicate-accessor", sym);
        ctx.Emit(sym, "as-err-property-duplicate-accessor", sym.name, "set");
    }

    const SymbolTable& table = ctx.request.symbolTable;

    if (IsMixinMember(sym, table))
    {
        ctx.LogRule("CheckVirtualProperty", "as-err-mixin-virtual-property", sym);
        ctx.Emit(sym, "as-err-mixin-virtual-property", sym.name);
    }

    // An interface declares accessors without bodies by definition, and so does a stub.
    if (HasContainerOfType(sym, table, SymbolType::Interface))
    {
        return;
    }

    if ((sig.hasGet && !sig.hasBodyGet) || (sig.hasSet && !sig.hasBodySet))
    {
        ctx.LogRule("CheckVirtualProperty", "as-err-property-accessor-missing-body", sym);
        ctx.Emit(sym, "as-err-property-accessor-missing-body", sym.name);
    }
}

/**
 * @brief Verifies that variable declaration modifiers comply with language rules (e.g. bans 'shared' on variables).
 * @param[in] sym Variable symbol being validated.
 * @param[in] sig Variable signature containing modifier flags.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
static void CheckModifiers(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.modifiers.isShared)
    {
        ctx.LogRule("CheckModifiers", "as-err-shared-not-allowed-on-entity", sym);
        ctx.Emit(sym, "as-err-shared-not-allowed-on-entity", sym.name);
    }
}

void ValidateVariable(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Variable && sym.type != SymbolType::Property)
    {
        return;
    }
    if (IsFromPredefinedStub(sym, ctx))
    {
        return;
    }
    if (!std::holds_alternative<VariableSignature>(sym.signature))
    {
        return;
    }

    const auto& sig = sym.GetVariable();

    if (sig.isLocal || sig.isEnumConstant || sym.containerKind == ContainerKind::Enum)
    {
        return;
    }

    // NOT IMPLEMENTED: as-err-standalone-reference.
    //
    // A standalone reference is only detectable through ParameterInformation::isStandaloneRef,
    // which exists for parameters and has no VariableSignature counterpart - a variable
    // declared `int &x;` reaches the collector with nothing recording the ampersand. Whether it
    // is even an error depends on asEP_ALLOW_UNSAFE_REFERENCES, a host build option no reader
    // of script text can observe.

    CheckModifiers(sym, sig, ctx);
    CheckTemplateArguments(sym, sig, ctx);
    CheckDeclaredType(sym, sig, ctx);
    CheckPlacement(sym, sig, ctx);
    CheckVirtualProperty(sym, sig, ctx);
    CheckEngineStorageRules(sym, sig, ctx);
    CheckEngineTypeSuggestions(sym, sig, ctx);
}

void CheckNullAssignedToNonHandle(const SymbolTable& symbolTable, DiagnosticContext& ctx)
{
    symbolTable.ForEachSymbolInFile(
        ctx.request.fileUri,
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri != ctx.request.fileUri)
                {
                    continue;
                }

                if (!std::holds_alternative<VariableSignature>(sym.signature))
                {
                    continue;
                }

                const VariableSignature& varSig = sym.GetVariable();
                if (varSig.isVirtualProperty || !varSig.hasNullInitializer || varSig.modifiers.isHandle)
                {
                    continue;
                }

                if (!IsUnconditionallyNonNullablePrimitive(varSig.typeKind))
                {
                    continue;
                }

                ctx.Emit(sym, "as-err-null-non-handle", varSig.typeName, DiagnosticSeverity::Error);
            }
        });
}

void CheckNullAssignedToNonHandleInScope(const Scope* rootScope, DiagnosticContext& ctx)
{
    if (!rootScope)
    {
        return;
    }

    std::vector<const Scope*> worklist{rootScope};
    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        bool isFunctionNested = false;
        for (const Scope* ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto& def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                {
                    continue;
                }

                if (!def.hasNullInitializer || def.isHandleType)
                {
                    continue;
                }

                if (!IsUnconditionallyNonNullablePrimitive(def.typeKind))
                {
                    continue;
                }

                ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                                "as-err-null-non-handle", def.typeName, DiagnosticSeverity::Error);
            }
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                worklist.push_back(child.get());
            }
        }
    }
}

} // namespace angel_lsp::analysis::rules
