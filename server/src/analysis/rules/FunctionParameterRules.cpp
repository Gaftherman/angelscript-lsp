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
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::rules
{
/**
 * @brief Reserved words rejected as parameter names by the real AngelScript compiler.
 *        MEASURED: Each keyword was probed against the AngelScript compiler via angelscript_oracle
 */
inline constexpr std::string_view k_reservedParameterNames[] = {
    "and",      "auto",    "bool",      "break",  "case",   "cast",      "catch", "class",   "const",
    "continue", "default", "do",        "double", "else",   "enum",      "false", "float",   "for",
    "foreach",  "funcdef", "if",        "import", "in",     "inout",     "int",   "int16",   "int32",
    "int64",    "int8",    "interface", "is",     "mixin",  "namespace", "not",   "null",    "or",
    "out",      "private", "protected", "return", "switch", "true",      "try",   "typedef", "uint",
    "uint16",   "uint32",  "uint64",    "uint8",  "using",  "void",      "while", "xor",
};

/**
 * @brief True when the word is in the 53-word measured rejected list for parameter names.
 */
[[nodiscard]] static bool IsReservedParameterName(std::string_view name) noexcept
{
    return std::binary_search(std::begin(k_reservedParameterNames), std::end(k_reservedParameterNames), name);
}

void CheckParamModifiers(const Symbol& sym, const ParameterInformation& param, const DiagnosticContext& ctx)
{
    if (param.modifier == ParameterModifier::Out)
    {
        if (param.isConst)
        {
            ctx.LogParam("ValidateParameters", "as-err-const-out-param", param, sym);
            ctx.Emit(param, sym, "as-err-const-out-param", param.name);
        }
        if (!param.defaultValue.empty() && CleanBaseType(param.defaultValue) != "void")
        {
            ctx.LogParam("ValidateParameters", "as-err-out-param-default", param, sym);
            ctx.Emit(param, sym, "as-err-out-param-default", param.name);
        }
    }

    const bool isInOutReference = param.modifier == ParameterModifier::InOut || param.isStandaloneRef;
    if (isInOutReference && !param.isArray && !ctx.request.AllowsUnsafeReferences() &&
        IsDisallowedInOutType(param, ctx.request.symbolTable))
    {
        ctx.LogParam("ValidateParameters", "as-err-inout-on-primitive", param, sym);
        ctx.Emit(param, sym, "as-err-inout-on-primitive", param.baseTypeName);
    }

    if (param.hasDoubleReference)
    {
        ctx.LogParam("ValidateParameters", "as-err-double-reference", param, sym);
        ctx.Emit(param, sym, "as-err-double-reference", param.baseTypeName);
    }

    if (param.hasPrimitiveHandle)
    {
        ctx.LogParam("ValidateParameters", "as-err-handle-on-primitive", param, sym);
        ctx.Emit(param, sym, "as-err-handle-on-primitive", param.baseTypeName);
    }
}

void CheckParamTypeLegality(const Symbol& sym, const ParameterInformation& param, const DiagnosticContext& ctx)
{
    if (IsMixinClass(param.baseTypeName, ctx.request.symbolTable))
    {
        ctx.LogParam("ValidateParameters", "as-err-mixin-not-a-type", param, sym);
        ctx.Emit(param, sym, "as-err-mixin-not-a-type", param.baseTypeName);
    }

    if (!param.isHandle && param.templateName.empty() && !param.isArray &&
        ClassifyNonInstantiable(param.baseTypeName, ctx.request.symbolTable) != NonInstantiableKind::None)
    {
        ctx.LogParam("ValidateParameters", "as-err-parameter-not-instantiable", param, sym);
        ctx.Emit(param, sym, "as-err-parameter-not-instantiable",
                 param.typeName.empty() ? param.baseTypeName : param.typeName);
    }

    if (!param.isHandle && param.templateName.empty() && !param.isArray && !param.baseTypeName.empty())
    {
        const auto typeSymbols = ctx.request.symbolTable.FindSymbolsPtr(param.baseTypeName);
        const bool isFuncdefType =
            typeSymbols && std::any_of(typeSymbols->begin(), typeSymbols->end(),
                                       [](const Symbol& type) { return type.type == SymbolType::Funcdef; });
        if (isFuncdefType)
        {
            ctx.LogParam("ValidateParameters", "as-err-funcdef-not-handle", param, sym);
            ctx.Emit(param, sym, "as-err-funcdef-not-handle", {param.baseTypeName, param.baseTypeName});
        }
    }
}

void CheckParamTypeResolution(const Symbol& sym, const ParameterInformation& param, const DiagnosticContext& ctx)
{
    const bool isPredefined =
        angel_lsp::utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension);
    const std::string_view enclosingNs = EnclosingNamespaceView(sym, ctx.request.symbolTable);
    const auto ruleIndex = ctx.request.symbolTable.GetRuleIndex();
    const bool existsInOtherScope = ruleIndex && ruleIndex->allNames.contains(param.baseTypeName);
    const bool judgeParameterType = (sym.type == SymbolType::Function && sym.GetFunction().isImported) ||
                                    ctx.request.ReportsUnknownTypes() || existsInOtherScope;
    if (judgeParameterType && !param.baseTypeName.empty() &&
        !(isPredefined && (param.baseTypeName == "?" || param.baseTypeName == "T")) &&
        !IsTemplateParameterOfContainer(param.baseTypeName, sym, ctx.request.symbolTable) &&
        !IsKnownType(param.baseTypeName, ctx, enclosingNs))
    {
        ctx.LogParam("ValidateParameters", "as-err-unresolved-type", param, sym);
        ctx.EmitAtTypeName(param, sym, "as-err-unresolved-type", param.baseTypeName);
    }
}

void CheckParamName(const Symbol& sym, const ParameterInformation& param,
                    ankerl::unordered_dense::set<std::string_view>& seenNames, const DiagnosticContext& ctx)
{
    if (param.name.empty())
    {
        return;
    }

    if (IsReservedParameterName(param.name))
    {
        ctx.LogParam("ValidateParameters", diagnostics::codes::ReservedWordAsParameterName, param, sym);
        ctx.Emit(param, sym, diagnostics::codes::ReservedWordAsParameterName, param.name);
        return;
    }

    if (!seenNames.insert(param.name).second)
    {
        ctx.LogParam("ValidateParameters", "as-err-duplicate-param", param, sym);
        ctx.Emit(param, sym, "as-err-duplicate-param", {param.name, sym.name});
    }
}

void ValidateParameters(const Symbol& sym, const std::vector<ParameterInformation>& parameters, bool isFuncdef,
                        const DiagnosticContext& ctx)
{
    ankerl::unordered_dense::set<std::string_view> seenNames;
    bool seenDefault = false;

    for (const auto& param : parameters)
    {
        if (!param.defaultValue.empty())
        {
            seenDefault = true;
        }
        else if (seenDefault && !isFuncdef)
        {
            ctx.LogParam("ValidateParameters", "as-err-default-param-order", param, sym);
            ctx.Emit(param, sym, "as-err-default-param-order", sym.name);
        }

        if (param.typeKind == TypeKind::Void && !(parameters.size() == 1 && param.name.empty()))
        {
            ctx.LogParam("ValidateParameters", "as-err-void-parameter", param, sym);
            ctx.Emit(param, sym, "as-err-void-parameter", {param.name, sym.name});
        }

        CheckParamModifiers(sym, param, ctx);
        CheckParamTypeLegality(sym, param, ctx);
        CheckParamTypeResolution(sym, param, ctx);
        CheckParamName(sym, param, seenNames, ctx);
    }
}

} // namespace angel_lsp::analysis::rules
