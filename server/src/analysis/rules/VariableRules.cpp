#include "analysis/rules/VariableRules.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "parser/ASTUtils.h"
#include "spdlog/fmt/fmt.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <string>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Looks up what kind of declaration a type name refers to, if this analyzer sees one.
 */
static const Symbol* FindTypeDeclaration(std::string_view typeName, const SymbolTable& table,
                                         std::shared_ptr<const std::vector<Symbol>>& keepAlive,
                                         std::string_view enclosingNs = {})
{
    if (typeName.empty())
    {
        return nullptr;
    }

    keepAlive = table.FindSymbolsPtr(typeName);
    if (!keepAlive && !enclosingNs.empty())
    {
        utils::ForEachEnclosingScope(enclosingNs,
                                     [&](std::string_view current)
                                     {
                                         keepAlive = table.FindMemberSymbolPtr(current, typeName);
                                         return keepAlive != nullptr;
                                     });
    }
    if (!keepAlive)
    {
        return nullptr;
    }

    for (const auto& sym : *keepAlive)
    {
        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface || sym.type == SymbolType::Funcdef ||
            sym.type == SymbolType::Enum || sym.type == SymbolType::Typedef)
        {
            return &sym;
        }
    }
    return nullptr;
}

/**
 * @brief True when the symbol's container is a class or interface rather than global/namespace.
 * @param[in] sym Target variable symbol.
 * @param[in] table Symbol table for container lookup.
 * @return True if enclosed within a class or interface.
 */
static bool IsClassOrInterfaceMember(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        return true;
    }
    if (sym.containerKind == ContainerKind::Namespace || sym.containerName.empty())
    {
        return false;
    }
    const auto container = table.FindSymbolsPtr(sym.containerName);
    return container && std::any_of(container->begin(), container->end(), [](const Symbol& owner)
                                    { return owner.type == SymbolType::Class || owner.type == SymbolType::Interface; });
}

/**
 * @brief True when the symbol's container is a mixin class.
 */
static bool IsMixinMember(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerName.empty())
    {
        return false;
    }
    const auto container = table.FindSymbolsPtr(sym.containerName);
    return container && std::any_of(container->begin(), container->end(), [](const Symbol& owner)
                                    { return owner.type == SymbolType::Class && owner.GetClass().modifiers.isMixin; });
}

/**
 * @brief Rules about the arguments a template type is instantiated with.
 *        Only one argument is invalid for every template, and it is invalid whatever the host
 */
static void CheckTemplateArguments(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.templateName.empty())
    {
        return;
    }

    for (const auto& argument : sig.templateArgumentTypes)
    {
        if (CleanBaseTypeView(argument) != "void")
        {
            continue;
        }

        ctx.LogRule("CheckTemplateArguments", "as-err-array-invalid-template", sym);
        ctx.Emit(sym, "as-err-array-invalid-template", fmt::format("{}<{}>", sig.templateName, argument));
    }
}

static void CheckPrimitiveOrVoidType(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.typeKind == TypeKind::Void && !sig.modifiers.isHandle)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-void-variable", sym);
        ctx.Emit(sym, "as-err-void-variable", sym.name);
    }

    if (sig.hasPrimitiveHandle)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", sig.baseTypeName);
    }
}

static void CheckNonInstantiableType(const Symbol& sym, const VariableSignature& sig, std::string_view baseType,
                                     const DiagnosticContext& ctx)
{
    if (sig.modifiers.isHandle || !sig.templateName.empty())
    {
        return;
    }

    switch (ClassifyNonInstantiable(baseType, ctx.request.symbolTable))
    {
    case NonInstantiableKind::Abstract:
        ctx.LogRule("CheckDeclaredType", "as-err-abstract-instantiated", sym);
        ctx.Emit(sym, "as-err-abstract-instantiated", baseType, baseType);
        break;
    case NonInstantiableKind::Interface:
        ctx.LogRule("CheckDeclaredType", "as-err-interface-instantiated", sym);
        ctx.Emit(sym, "as-err-interface-instantiated", baseType, baseType);
        break;
    case NonInstantiableKind::Mixin:
        ctx.LogRule("CheckDeclaredType", "as-err-mixin-not-a-type", sym);
        ctx.Emit(sym, "as-err-mixin-not-a-type", baseType);
        break;
    case NonInstantiableKind::None:
        break;
    }
}

static void CheckUnresolvedVariableType(const Symbol& sym, std::string_view baseType, std::string_view enclosingNs,
                                        const DiagnosticContext& ctx)
{
    if (baseType.empty() || baseType == "auto" || IsReservedKeyword(std::string(baseType)))
    {
        return;
    }
    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    if (isPredefined && (baseType == "?" || baseType == "T"))
    {
        return;
    }
    if (IsTemplateParameterOfContainer(baseType, sym, ctx.request.symbolTable) ||
        IsKnownType(std::string(baseType), ctx, enclosingNs))
    {
        return;
    }

    const auto ruleIndex = ctx.request.symbolTable.GetRuleIndex();
    const bool existsInOtherScope = ruleIndex && ruleIndex->allNames.contains(std::string(baseType));
    if (ctx.request.ReportsUnknownTypes() || existsInOtherScope)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-unresolved-type", sym);
        ctx.Emit(sym, "as-err-unresolved-type", baseType);
    }
}

/**
 * @brief Rules about what the declared type may be.
 */
static void CheckDeclaredType(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    CheckPrimitiveOrVoidType(sym, sig, ctx);

    const std::string_view baseType = CleanBaseTypeView(sig.baseTypeName.empty() ? sig.typeName : sig.baseTypeName);
    const std::string enclosingNs = EnclosingNamespaceForSymbol(sym, ctx.request.symbolTable);
    std::shared_ptr<const std::vector<Symbol>> keepAlive;
    const Symbol* declaration = FindTypeDeclaration(baseType, ctx.request.symbolTable, keepAlive, enclosingNs);
    if (!declaration)
    {
        CheckUnresolvedVariableType(sym, baseType, enclosingNs, ctx);
        return;
    }

    if (!sig.isArray && sig.templateName.empty() && declaration->type == SymbolType::Funcdef && !sig.modifiers.isHandle)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-funcdef-not-handle", sym);
        ctx.Emit(sym, "as-err-funcdef-not-handle", baseType, baseType);
    }

    if (declaration->type == SymbolType::Class && declaration->GetClass().modifiers.isMixin)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-mixin-not-a-type", sym);
        ctx.Emit(sym, "as-err-mixin-not-a-type", baseType);
    }

    // Declaring one by value makes an instance of it, which is the one thing an abstract
    // class and an interface exist not to allow. A handle is not an instance and is the
    // whole point of both, so `Shape@ s;` is legal - and a reference does not save it,
    // since even `const Shape &in` is refused as a parameter.
    //
    // A template instantiation is not judged at all. baseType is the *element* there,
    // because CleanBaseType unwraps `array<T>` to T, and the engine decides a subtype by
    // the factory registered for it rather than by whether it is abstract: `array<Shape>`
    // by value compiles, while `array<IThing>` fails with a message about a missing
    // default factory rather than about instantiating an interface. Found by the corpus
    // audit, which flagged `array<IContext@>` - ordinary, correct code.
    if (IsMixinClass(baseType, ctx.request.symbolTable))
    {
        ctx.LogRule("CheckDeclaredType", "as-err-mixin-not-a-type", sym);
        ctx.Emit(sym, "as-err-mixin-not-a-type", baseType);
        return;
    }

    CheckNonInstantiableType(sym, sig, baseType, ctx);
}

/**
 * @brief Rules about the modifiers a declaration carries where it sits.
 */
static void CheckPlacement(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
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

    // Function-body locals are not reached through the symbol table at all; anything marked
    // local here came in some other way and is not this module's business.
    if (sig.isLocal)
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
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<Symbol>& symbols)
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

static SourceRange GetDefinitionTypeRange(const LocalDefinition& def)
{
    const bool hasExplicitType = (def.typeEndCharacter > def.typeStartCharacter || def.typeEndLine > def.typeStartLine);
    return SourceRange{
        hasExplicitType ? def.typeStartLine : def.startLine,
        hasExplicitType ? def.typeStartCharacter : def.startCharacter,
        hasExplicitType ? def.typeEndLine : def.endLine,
        hasExplicitType ? def.typeEndCharacter : def.endCharacter,
    };
}

static void ValidateTemplateArguments(const LocalDefinition& def, const TemplateTypeInfo& tmplInfo,
                                      std::string_view enclosingNs, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    if (!IsKnownType(tmplInfo.containerName, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", tmplInfo.containerName, DiagnosticSeverity::Error);
    }
    for (size_t i = 0; i < tmplInfo.templateArgs.size(); ++i)
    {
        std::string cleanArg = CleanBaseType(tmplInfo.templateArgs[i]);
        if (!IsKnownType(cleanArg, ctx, enclosingNs))
        {
            uint32_t sLine = range.startLine;
            uint32_t sChar = range.startCharacter;
            uint32_t eLine = range.endLine;
            uint32_t eChar = range.endCharacter;

            if (i < def.templateArgPositions.size())
            {
                sLine = def.templateArgPositions[i].startLine;
                sChar = def.templateArgPositions[i].startCharacter;
                eLine = def.templateArgPositions[i].endLine;
                eChar = def.templateArgPositions[i].endCharacter;
            }

            ctx.EmitAtRange({sLine, sChar, eLine, eChar}, "as-err-unresolved-type", cleanArg,
                            DiagnosticSeverity::Error);
        }
    }
}

static bool CheckMissingFuncdefHint(const std::string& base, const SourceRange& range, DiagnosticContext& ctx)
{
    if (ctx.request.diagnostics && ctx.request.diagnostics->reportMissingFuncdef && base != "auto" &&
        NamesAFunctionNotAType(base, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(range, "as-hint-funcdef-missing", base, DiagnosticSeverity::Hint);
        return true;
    }
    return false;
}

static bool CheckIllegalHandleOnPrimitive(const LocalDefinition& def, const std::string& base, const SourceRange& range,
                                          DiagnosticContext& ctx)
{
    if (def.isHandleType && def.typeKind != TypeKind::Array && base != "auto" && IsPrimitiveTypeName(base))
    {
        ctx.EmitAtRange(range, "as-err-handle-on-primitive", base, DiagnosticSeverity::Error);
        return true;
    }
    return false;
}

static std::string ResolveEnclosingNamespaceForDefinition(const LocalDefinition& def, const DiagnosticContext& ctx)
{
    if (!ctx.request.tree || ctx.request.sourceCode.empty())
    {
        return "";
    }
    const TSNode node = ts_node_descendant_for_point_range(
        ts_tree_root_node(ctx.request.tree), {def.startLine, def.startCharacter}, {def.endLine, def.endCharacter});
    if (ts_node_is_null(node))
    {
        return "";
    }
    for (const auto& container : GetEnclosingContainers(node, ctx.request.sourceCode))
    {
        if (container.kind == ContainerKind::Namespace)
        {
            return container.qualifiedName;
        }
    }
    return "";
}

static void ValidateVariableType(const LocalDefinition& def, DiagnosticContext& ctx)
{
    const SourceRange range = GetDefinitionTypeRange(def);
    const std::string base = CleanBaseType(def.typeName);

    if (def.typeName == "void" || base == "void")
    {
        if (def.kind == LocalDefinitionKind::Parameter)
        {
            ctx.EmitAtRange(range, "as-err-void-parameter", {def.name, "<anonymous>"}, DiagnosticSeverity::Error);
        }
        else
        {
            ctx.EmitAtRange(range, "as-err-void-variable", def.name, DiagnosticSeverity::Error);
        }
        return;
    }
    if (IsMixinClass(base, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(range, "as-err-mixin-not-a-type", base, DiagnosticSeverity::Error);
        return;
    }
    if (CheckMissingFuncdefHint(base, range, ctx) || CheckIllegalHandleOnPrimitive(def, base, range, ctx))
    {
        return;
    }

    const std::string enclosingNs = ResolveEnclosingNamespaceForDefinition(def, ctx);
    const TemplateTypeInfo tmplInfo = ParseTemplateType(def.typeName);
    if (!tmplInfo.templateArgs.empty())
    {
        ValidateTemplateArguments(def, tmplInfo, enclosingNs, ctx);
    }
    else if (!base.empty() && base != "auto" && !IsReservedKeyword(base) && !IsKnownType(base, ctx, enclosingNs))
    {
        ctx.EmitAtRange(range, "as-err-unresolved-type", base, DiagnosticSeverity::Error);
    }

    CheckEngineLocalTypeSuggestions(def, ctx);
}

void CheckLocalNames(const Scope* scope, DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string> declaredHere;

    if (scope->parent && scope->parent->isFunctionScope)
    {
        for (const auto& param : scope->parent->definitions)
        {
            if (param.kind == LocalDefinitionKind::Parameter)
            {
                declaredHere.insert(param.name);
            }
        }
    }

    for (const auto& def : scope->definitions)
    {
        if (def.kind != LocalDefinitionKind::Variable && def.kind != LocalDefinitionKind::Parameter)
        {
            continue;
        }

        if (!declaredHere.insert(def.name).second)
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            "as-err-duplicate-symbol", def.name);
        }

        if (IsReservedKeyword(def.name) && def.kind == LocalDefinitionKind::Variable &&
            IsKnownType(CleanBaseType(def.typeName), ctx))
        {
            ctx.EmitAtRange({def.startLine, def.startCharacter, def.endLine, def.endCharacter},
                            "as-err-reserved-keyword-name", def.name);
        }
    }
}

void CheckLocalTypes(const Scope* scope, DiagnosticContext& ctx)
{
    for (const auto& def : scope->definitions)
    {
        if (def.kind == LocalDefinitionKind::Variable ||
            (scope->kind == ScopeKind::Closure && def.kind == LocalDefinitionKind::Parameter))
        {
            ValidateVariableType(def, ctx);
        }
    }
}

void CheckLocalVariableDeclarations(const Scope* rootScope, DiagnosticContext& ctx)
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
            CheckLocalNames(scope, ctx);
            CheckLocalTypes(scope, ctx);
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
