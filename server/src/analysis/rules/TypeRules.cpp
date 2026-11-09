#include "analysis/rules/TypeRules.h"

#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/EnumDeclarationFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief True when the name collides with a keyword or a built-in type name.
 */
bool IsUnusableName(std::string_view name, const DiagnosticContext& ctx)
{
    return IsReservedKeyword(name) || IsPrimitiveTypeName(name) || name == ctx.request.GetEffectiveStringTypeName() ||
           name == ctx.request.GetEffectiveArrayTypeName();
}

/**
 * @brief Formats a symbol type into the noun the compiler uses in as-err-name-conflict.
 */
std::string_view KindWord(SymbolType type)
{
    switch (type)
    {
    case SymbolType::Function:
        return "function";
    case SymbolType::Class:
        return "class";
    case SymbolType::Interface:
        return "interface";
    case SymbolType::Enum:
        return "enum";
    case SymbolType::Typedef:
        return "typedef";
    case SymbolType::Funcdef:
        return "funcdef";
    case SymbolType::Namespace:
        return "namespace";
    default:
        return "variable";
    }
}

void ValidateTypedef(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Typedef)
    {
        return;
    }

    if (IsUnusableName(sym.name, ctx))
    {
        ctx.LogRule("ValidateTypedef", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    const auto& sig = sym.GetTypedef();
    const std::string_view baseType = CleanBaseTypeView(sig.baseType);

    if (!sig.hasSemicolon)
    {
        ctx.LogRule("ValidateTypedef", "as-syntax-error-missing", sym);
        ctx.Emit(sym, "as-syntax-error-missing", ";");
    }

    if (!baseType.empty() && !IsPrimitiveTypeName(baseType))
    {
        ctx.LogRule("ValidateTypedef", diagnostics::codes::TypedefNonPrimitive, sym);
        if (sig.baseTypeEndCharacter > sig.baseTypeStartCharacter || sig.baseTypeEndLine > sig.baseTypeStartLine)
        {
            ctx.EmitAtRange(
                {sig.baseTypeStartLine, sig.baseTypeStartCharacter, sig.baseTypeEndLine, sig.baseTypeEndCharacter},
                diagnostics::codes::TypedefNonPrimitive, baseType);
        }
        else
        {
            ctx.Emit(sym, diagnostics::codes::TypedefNonPrimitive, baseType);
        }
    }
}

static void ValidateFuncdefReturnType(const Symbol& sym, const FuncdefSignature& sig, const DiagnosticContext& ctx)
{
    if (sig.returnHasPrimitiveHandle)
    {
        ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", sig.returnBaseTypeName);
    }

    const std::string_view retBase =
        CleanBaseTypeView(sig.returnBaseTypeName.empty() ? sig.returnType : sig.returnBaseTypeName);
    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    const std::string_view enclosingNs = EnclosingNamespaceView(sym, ctx.request.symbolTable);
    if (!retBase.empty() && retBase != "void" && retBase != "auto" &&
        !(isPredefined && (retBase == "?" || retBase == "T")) &&
        !IsTemplateParameterOfContainer(retBase, sym, ctx.request.symbolTable) &&
        !IsKnownType(retBase, ctx, enclosingNs))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
        if (sig.returnTypeEndCharacter > sig.returnTypeStartCharacter ||
            sig.returnTypeEndLine > sig.returnTypeStartLine)
        {
            ctx.EmitAtRange({sig.returnTypeStartLine, sig.returnTypeStartCharacter, sig.returnTypeEndLine,
                             sig.returnTypeEndCharacter},
                            "as-err-unresolved-type", retBase);
        }
        else
        {
            ctx.Emit(sym, "as-err-unresolved-type", retBase);
        }
    }
}

static void ValidateFuncdefParameter(const Symbol& sym, const ParameterInformation& param, const DiagnosticContext& ctx)
{
    if (param.hasPrimitiveHandle)
    {
        ctx.LogRule("ValidateFuncdef", "as-err-handle-on-primitive", sym);
        ctx.Emit(sym, "as-err-handle-on-primitive", param.baseTypeName);
    }

    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    const std::string_view paramBase =
        CleanBaseTypeView(param.baseTypeName.empty() ? param.typeName : param.baseTypeName);
    const std::string_view enclosingNs = EnclosingNamespaceView(sym, ctx.request.symbolTable);
    if (!paramBase.empty() && paramBase != "void" && paramBase != "auto" &&
        !(isPredefined && (paramBase == "?" || paramBase == "T")) &&
        !IsTemplateParameterOfContainer(paramBase, sym, ctx.request.symbolTable) &&
        !IsKnownType(paramBase, ctx, enclosingNs))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-unresolved-type", sym);
        if (param.endCharacter > param.startCharacter || param.endLine > param.startLine)
        {
            ctx.EmitAtRange({param.startLine, param.startCharacter, param.endLine, param.endCharacter},
                            "as-err-unresolved-type", paramBase);
        }
        else
        {
            ctx.Emit(sym, "as-err-unresolved-type", paramBase);
        }
    }
}

static void ValidateFuncdefParameters(const Symbol& sym, const std::vector<ParameterInformation>& params,
                                      const DiagnosticContext& ctx)
{
    for (const auto& param : params)
    {
        ValidateFuncdefParameter(sym, param, ctx);
    }
}

void ValidateFuncdef(const Symbol& sym, const DiagnosticContext& ctx)
{
    if (sym.type != SymbolType::Funcdef)
    {
        return;
    }

    if (IsUnusableName(sym.name, ctx))
    {
        ctx.LogRule("ValidateFuncdef", "as-err-reserved-keyword-name", sym);
        ctx.Emit(sym, "as-err-reserved-keyword-name", sym.name);
        return;
    }

    const auto& sig = sym.GetFuncdef();

    const std::string_view attribute = FirstAttributeName(sig.modifiers);
    if (!attribute.empty())
    {
        ctx.LogRule("ValidateFuncdef", "as-err-funcdef-attribute", sym);
        ctx.Emit(sym, "as-err-funcdef-attribute", attribute, sym.name);
    }

    ValidateFuncdefReturnType(sym, sig, ctx);
    ValidateFuncdefParameters(sym, sig.parameters, ctx);
}

} // namespace angel_lsp::analysis::rules
