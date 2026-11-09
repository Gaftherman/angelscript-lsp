#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/RuleSyntaxFacts.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{

FunctionContext BuildFunctionContext(const Symbol& sym, const DiagnosticContext& ctx)
{
    FunctionContext fctx;
    if (sym.containerName.empty() || sym.containerKind == ContainerKind::Namespace)
    {
        // No container or namespace container: this is a non-member (global or namespace function).
        fctx.containerIsKnown = true;
        return fctx;
    }

    if (sym.containerKind == ContainerKind::Class || sym.containerKind == ContainerKind::Interface)
    {
        fctx.containerIsKnown = true;
        fctx.isMember = true;
        fctx.isInterface = (sym.containerKind == ContainerKind::Interface);
    }

    const auto container = ctx.request.symbolTable.FindSymbolsPtr(sym.containerName);
    if (container)
    {
        fctx.containerIsKnown = true;
        for (const auto& owner : *container)
        {
            if (owner.type == SymbolType::Interface)
            {
                fctx.isMember = true;
                fctx.isInterface = true;
            }
            else if (owner.type == SymbolType::Class)
            {
                fctx.isMember = true;
                fctx.isMixin = fctx.isMixin || owner.GetClass().modifiers.isMixin;
            }
        }
    }

    if (fctx.isMember)
    {
        fctx.isDestructor = IsDestructorDeclaration(sym, ctx);
        // A constructor is spelled with the class's own name. The container's qualified name
        // is what the collector stores, so compare against its last segment.
        fctx.ownerName = LastScopeSegment(sym.containerName);
        fctx.isConstructor = !fctx.isDestructor && sym.name == fctx.ownerName;
    }

    return fctx;
}

/**
 * @brief True when the type cannot be passed as &inout without unsafe references.
 */
bool IsDisallowedInOutType(const ParameterInformation& param, const SymbolTable& table)
{
    if (IsValueOnlyKind(param.typeKind) || param.typeKind == TypeKind::String)
    {
        return true;
    }
    const auto unwrapped = UnwrapTypedef(param.baseTypeName, table);
    if (parser::primitives::IsPrimitive(unwrapped) || unwrapped == table.GetStringType())
    {
        return true;
    }
    if (auto syms = table.FindSymbolsPtr(unwrapped))
    {
        return std::any_of(syms->begin(), syms->end(), [](const Symbol& s) { return s.type == SymbolType::Enum; });
    }
    return false;
}

// Body

/**
 * @brief True when a bodiless declaration cannot be read as a variable with a constructor
 *        initializer, and so really is a function prototype.
 */
static bool IsUnmistakablyAPrototype(const FunctionSignature& sig)
{
    if (sig.returnTypeKind == TypeKind::Void)
    {
        return true;
    }
    return std::any_of(sig.parameters.begin(), sig.parameters.end(), [](const ParameterInformation& param)
                       { return !param.name.empty() || param.typeKind == TypeKind::Void; });
}

static bool MayOmitBody(const FunctionSignature& sig, const FunctionContext& fctx)
{
    return sig.isInterfaceMethod || fctx.isInterface || sig.modifiers.isExternal || sig.modifiers.isDelete ||
           fctx.isConstructor || fctx.isDestructor || sig.isImported;
}

static void CheckDeleteModifiers(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.hasBody && sig.modifiers.isDelete)
    {
        ctx.LogRule("CheckBody", "as-err-delete-with-body", sym);
        ctx.Emit(sym, "as-err-delete-with-body", sym.name);
    }

    if (sig.modifiers.isDelete && (sig.modifiers.isConst || sig.modifiers.isFinal || sig.modifiers.isOverride))
    {
        ctx.LogRule("CheckBody", "as-err-delete-with-other-qualifier", sym);
        ctx.Emit(sym, "as-err-delete-with-other-qualifier", sym.name);
    }
}

void CheckBody(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
               const DiagnosticContext& ctx)
{
    if (sig.isImported && sig.hasBody)
    {
        ctx.LogRule("CheckBody", "as-err-import-has-body", sym);
        ctx.Emit(sym, "as-err-import-has-body", sym.name);
        return;
    }

    if (!sig.hasBody && !MayOmitBody(sig, fctx) && IsUnmistakablyAPrototype(sig))
    {
        ctx.LogRule("CheckBody", "as-err-missing-body", sym);
        ctx.Emit(sym, "as-err-missing-body", sym.name);
    }

    CheckDeleteModifiers(sym, sig, ctx);
}

// Return type

static void CheckVoidReturnType(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.returnTypeKind != TypeKind::Void)
    {
        return;
    }

    if (sig.returnIsConst)
    {
        ctx.LogRule("CheckReturnType", "as-err-const-void-return", sym);
        ctx.Emit(sym, "as-err-const-void-return");
    }

    if (sig.modifiers.isReturnReference)
    {
        ctx.LogRule("CheckReturnType", "as-err-void-reference", sym);
        ctx.Emit(sym, "as-err-void-reference");
    }
}

/**
 * @brief Enforces AngelScript rules on function return types (bans primitive handles and illegal return types).
 * @param[in] sym Function symbol being checked.
 * @param[in] sig Function signature containing return type and modifiers.
 * @param[in] ctx Diagnostic collection and analysis context.
 */
static void CheckReturnTypeLegality(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.returnHasPrimitiveHandle && !sig.returnIsArray)
    {
        ctx.LogRule("CheckReturnType", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
    }

    if (IsMixinClass(sig.returnBaseTypeName, ctx.request.symbolTable))
    {
        ctx.LogRule("CheckReturnType", "as-err-mixin-not-a-type", sym);
        ctx.Emit(sym, "as-err-mixin-not-a-type", sig.returnBaseTypeName);
    }

    const bool returnsByValue =
        !IsHandleTypeView(sig.returnType) && !IsReferenceTypeView(sig.returnType) && !sig.modifiers.isReturnReference;
    if (returnsByValue && !sig.returnIsArray && sig.returnTemplateName.empty() &&
        ClassifyNonInstantiable(sig.returnBaseTypeName, ctx.request.symbolTable) != NonInstantiableKind::None)
    {
        ctx.LogRule("CheckReturnType", "as-err-return-not-instantiable", sym);
        ctx.Emit(sym, "as-err-return-not-instantiable", sig.returnBaseTypeName);
    }
}

static void CheckReturnTypeResolution(const Symbol& sym, const FunctionSignature& sig, const DiagnosticContext& ctx)
{
    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    const std::string_view enclosingNs = EnclosingNamespaceView(sym, ctx.request.symbolTable);
    const auto ruleIndex = ctx.request.symbolTable.GetRuleIndex();
    const bool existsInOtherScope = ruleIndex && ruleIndex->allNames.contains(sig.returnBaseTypeName);
    if ((sig.isImported || ctx.request.ReportsUnknownTypes() || existsInOtherScope) &&
        !sig.returnBaseTypeName.empty() && sig.returnBaseTypeName != "void" &&
        !(isPredefined && (sig.returnBaseTypeName == "?" || sig.returnBaseTypeName == "T")) &&
        !IsTemplateParameterOfContainer(sig.returnBaseTypeName, sym, ctx.request.symbolTable) &&
        !IsKnownType(sig.returnBaseTypeName, ctx, enclosingNs))
    {
        ctx.LogRule("CheckReturnType", "as-err-unresolved-type", sym);
        ctx.EmitAtTypeName(sym, "as-err-unresolved-type", sig.returnBaseTypeName);
    }
}

void CheckReturnType(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                     const DiagnosticContext& ctx)
{
    // A constructor and a destructor have no return type to judge, and what the collector
    // put in returnType for them is whatever preceded the name.
    if (fctx.isConstructor || fctx.isDestructor)
    {
        return;
    }

    CheckVoidReturnType(sym, sig, ctx);
    CheckReturnTypeLegality(sym, sig, ctx);
    CheckReturnTypeResolution(sym, sig, ctx);
}

// Modifiers and placement

void CheckModifiers(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                    const DiagnosticContext& ctx)
{
    // Only judged when the container is known not to be a class: an unresolved container
    // means the analyzer cannot tell what this declaration is.
    if (!fctx.containerIsKnown || fctx.isMember)
    {
        return;
    }

    // Access modifiers outside a class:
    if (sig.modifiers.access != AccessModifier::Public)
    {
        ctx.LogRule("CheckModifiers", diagnostics::codes::GlobalFunctionAccessModifier, sym);
        ctx.Emit(sym, diagnostics::codes::GlobalFunctionAccessModifier, sym.name);
    }

    // `const` on a global function is a hard error - the engine's parser refuses the token
    // outright, answering "Instead found reserved keyword 'const'".
    if (sig.modifiers.isConst)
    {
        ctx.LogRule("CheckModifiers", "as-err-global-function-qualifiers", sym);
        ctx.Emit(sym, "as-err-global-function-qualifiers", sym.name);
    }

    // In AngelScript, `override` and `final` on a global function are compile errors:
    // "Unexpected token 'final' / 'override'".
    if (sig.modifiers.isOverride)
    {
        ctx.LogRule("CheckModifiers", diagnostics::codes::GlobalFunctionAttribute, sym);
        ctx.Emit(sym, diagnostics::codes::GlobalFunctionAttribute, {"override", sym.name}, DiagnosticSeverity::Error);
    }
    if (sig.modifiers.isFinal)
    {
        ctx.LogRule("CheckModifiers", diagnostics::codes::GlobalFunctionAttribute, sym);
        ctx.Emit(sym, diagnostics::codes::GlobalFunctionAttribute, {"final", sym.name}, DiagnosticSeverity::Error);
    }
}

// Function attributes: explicit, property, delete

/**
 * @brief True when the deleted declaration names a function the engine auto generates.
 *        Only three exist for a script class - the default constructor, the copy constructor, and
 */
static bool IsAutoGeneratedFunction(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx)
{
    const bool takesOwnTypeOnce =
        sig.parameters.size() == 1 && CleanBaseType(sig.parameters.front().baseTypeName) == fctx.ownerName;

    if (fctx.isConstructor)
    {
        return sig.parameters.empty() || takesOwnTypeOnce;
    }
    return sym.name == "opAssign" && takesOwnTypeOnce;
}

/**
 * @brief True when a virtual property accessor's signature is one the engine accepts.
 */
static bool IsValidAccessorSignature(std::string_view name, const FunctionSignature& sig)
{
    // An accessor may carry a leading index parameter, which is how `a[i].prop` and the
    // corpus's own `get_stringArray(int idx)` are written. That is the only reason a getter
    // has any parameters at all and the only reason a setter has two.
    constexpr size_t k_maxIndexParams = 1;

    if (name.rfind("get_", 0) == 0)
    {
        return sig.returnTypeKind != TypeKind::Void && sig.parameters.size() <= k_maxIndexParams;
    }
    if (name.rfind("set_", 0) == 0)
    {
        return sig.returnTypeKind == TypeKind::Void && !sig.parameters.empty() &&
               sig.parameters.size() <= k_maxIndexParams + 1;
    }
    // Anything else is not an accessor at all, whatever it claims.
    return false;
}

void CheckAttributes(const Symbol& sym, const FunctionSignature& sig, const FunctionContext& fctx,
                     const DiagnosticContext& ctx)
{
    // An interface declares a contract, not an implementation, and the engine rejects every
    // one of the five attributes on a method of one. The grammar parses them so this can
    // name the offender instead of leaving a syntax error on the token.
    if (sig.isInterfaceMethod)
    {
        const std::string_view attribute = FirstAttributeName(sig.modifiers);
        if (!attribute.empty())
        {
            ctx.LogRule("CheckAttributes", "as-err-interface-method-attribute", sym);
            ctx.Emit(sym, "as-err-interface-method-attribute", attribute, sym.name);
        }
        return;
    }

    // `explicit` suppresses an implicit conversion through a constructor, so it only means
    // anything inside a class body; the engine's parser refuses the token anywhere else.
    if (sig.modifiers.isExplicit && fctx.containerIsKnown && !fctx.isMember)
    {
        ctx.LogRule("CheckAttributes", "as-err-explicit-not-member", sym);
        ctx.Emit(sym, "as-err-explicit-not-member", sym.name);
    }

    if (sig.modifiers.isProperty && !IsValidAccessorSignature(sym.name, sig))
    {
        ctx.LogRule("CheckAttributes", "as-err-virtual-property-signature", sym);
        ctx.Emit(sym, "as-err-virtual-property-signature", sym.name);
    }

    // A destructor carrying `delete` is already reported, with a message that says so; this
    // one would only repeat it less precisely. The container has to be known too, since an
    // unresolved one leaves isConstructor false and would turn every deleted constructor in
    // an unreadable hierarchy into a finding.
    if (sig.modifiers.isDelete && fctx.containerIsKnown && !fctx.isDestructor &&
        !IsAutoGeneratedFunction(sym, sig, fctx))
    {
        ctx.LogRule("CheckAttributes", "as-err-delete-not-auto-generated", sym);
        ctx.Emit(sym, "as-err-delete-not-auto-generated", sym.name);
    }
}

// Constructors and destructors

} // namespace angel_lsp::analysis::rules
