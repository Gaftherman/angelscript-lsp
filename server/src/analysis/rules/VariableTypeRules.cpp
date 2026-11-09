#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/EngineRuleChecker.h"
#include "analysis/rules/VariableRules.h"
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
bool IsClassOrInterfaceMember(const Symbol& sym, const SymbolTable& table)
{
    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        return true;
    }
    if (sym.containerKind == ContainerKind::Namespace || sym.containerKind == ContainerKind::Enum ||
        sym.containerName.empty())
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
bool IsMixinMember(const Symbol& sym, const SymbolTable& table)
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
void CheckTemplateArguments(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
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
    if (sig.modifiers.isHandle || !sig.templateName.empty() || sig.isArray)
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

static void CheckUnresolvedVariableType(const Symbol& sym, const VariableSignature& sig, std::string_view baseType,
                                        std::string_view enclosingNs, const DiagnosticContext& ctx)
{
    if (baseType.empty() || baseType == "auto" || IsReservedKeyword(baseType))
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
        IsKnownType(baseType, ctx, enclosingNs))
    {
        return;
    }

    const auto ruleIndex = ctx.request.symbolTable.GetRuleIndex();
    const bool existsInOtherScope = ruleIndex && ruleIndex->allNames.contains(baseType);
    if (ctx.request.ReportsUnknownTypes() || existsInOtherScope)
    {
        ctx.LogRule("CheckDeclaredType", "as-err-unresolved-type", sym);
        const SourceRange targetRange = (sig.typeRange.endLine > sig.typeRange.startLine ||
                                         sig.typeRange.endCharacter > sig.typeRange.startCharacter)
                                            ? sig.typeRange
                                            : sym.selectionRange;
        ctx.EmitAtRange(sym, targetRange, "as-err-unresolved-type", baseType);
    }
}

/**
 * @brief Rules about what the declared type may be.
 */
void CheckDeclaredType(const Symbol& sym, const VariableSignature& sig, const DiagnosticContext& ctx)
{
    CheckPrimitiveOrVoidType(sym, sig, ctx);

    const std::string_view baseType = CleanBaseTypeView(sig.baseTypeName.empty() ? sig.typeName : sig.baseTypeName);
    const std::string_view enclosingNs = EnclosingNamespaceView(sym, ctx.request.symbolTable);
    std::shared_ptr<const std::vector<Symbol>> keepAlive;
    const Symbol* declaration = FindTypeDeclaration(baseType, ctx.request.symbolTable, keepAlive, enclosingNs);
    if (!declaration)
    {
        CheckUnresolvedVariableType(sym, sig, baseType, enclosingNs, ctx);
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

} // namespace angel_lsp::analysis::rules
