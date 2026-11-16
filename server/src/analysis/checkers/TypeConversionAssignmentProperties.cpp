#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/PropertyAndDelegateResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"
#include "utils/AccessorNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Extracts the assigned-value parameter from matching `set_` accessor symbols.
 * @param[in] accessors Candidate property accessor symbols.
 * @param[in] isIndexed True when the assignment LHS uses index brackets.
 * @param[in] binding Template parameter binding for the receiver type, if any.
 * @param[in] table Symbol table for container qualification.
 * @return Extracted value parameters from matching `set_` overloads.
 */
static std::vector<ParameterInformation> ExtractSetterParams(const std::vector<Symbol>& accessors, bool isIndexed,
                                                             const TemplateBinding& binding, const SymbolTable& table)
{
    std::vector<ParameterInformation> result;
    for (const auto& sym : accessors)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature) ||
            !sym.name.starts_with(utils::k_setterPrefix))
        {
            continue;
        }
        const auto& params = sym.GetFunction().parameters;
        const bool arityMatches = isIndexed ? (params.size() >= 2) : AcceptsSingleArgument(params);
        if (!arityMatches)
        {
            continue;
        }
        ParameterInformation param = isIndexed ? params.back() : params.front();
        if (!sym.containerName.empty())
        {
            param.typeName = QualifyTypeInContainer(param.typeName, sym.containerName, table);
        }
        if (binding.usable)
        {
            param.typeName = SubstituteTemplateParameters(param.typeName, binding);
        }
        result.push_back(std::move(param));
    }
    return result;
}

/**
 * @brief Collects the value parameter(s) of a property assignment target from `set_X` or fallback getter type.
 * @param[in] input Assignment site, property info, and fallback resolved left type.
 * @param[in] ctx Diagnostic context.
 * @return Parameter specifications representing valid destination types for the property setter.
 */
static std::vector<ParameterInformation> CollectPropertySetterParams(const AssignmentConversionInput& input,
                                                                     const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    const PropertyAccessInfo& pInfo = input.propertyInfo;
    std::vector<ParameterInformation> result;

    if (input.site.leftProperty.kind == PropertyAccessKind::Member)
    {
        std::string cleanObj = MemberOwnerType(pInfo.receiverType, ctx.request.GetEffectiveArrayTypeName());
        if (cleanObj.empty())
        {
            cleanObj = CleanBaseType(pInfo.receiverType);
        }
        const TemplateBinding binding = (pInfo.receiverType.find('<') != std::string_view::npos)
                                            ? BindTemplateArguments(pInfo.receiverType, table)
                                            : TemplateBinding{};
        char accBuf[128];
        const std::string_view setterName = utils::ComposeAccessorName(utils::k_setterPrefix, pInfo.propName, accBuf);
        if (!setterName.empty())
        {
            for (const auto& cls : GetInheritedTypeHierarchy(cleanObj, table))
            {
                if (const auto syms = table.FindMemberSymbolPtr(cls, setterName))
                {
                    result = ExtractSetterParams(*syms, input.site.leftIsIndex, binding, table);
                    if (!result.empty())
                    {
                        return result;
                    }
                }
            }
        }
    }
    else if (input.site.leftProperty.kind == PropertyAccessKind::Name)
    {
        const std::string_view scopeName = utils::SplitScopedName(input.site.leftProperty.text).first;
        if (!scopeName.empty())
        {
            const auto accessors = FindPropertyAccessors(TrimString(scopeName), pInfo.propName, table, false);
            result = ExtractSetterParams(accessors, input.site.leftIsIndex, TemplateBinding{}, table);
        }
        else
        {
            const auto globalAcc = FindGlobalPropertyAccessors(pInfo.propName, table, false);
            result = ExtractSetterParams(globalAcc, input.site.leftIsIndex, TemplateBinding{}, table);
            if (result.empty() && ctx.request.nodeIndex != nullptr)
            {
                const auto containers = ctx.request.nodeIndex->CollectLookupContainers(
                    input.site.leftProperty.bytes.startByte, input.site.leftProperty.bytes.endByte,
                    input.site.leftProperty.nodeSymbol, ctx.request.sourceCode);
                for (const auto& cont : containers)
                {
                    const auto& cName = cont.qualifiedName.empty() ? cont.name : cont.qualifiedName;
                    const auto contAcc = FindPropertyAccessors(cName, pInfo.propName, table, false);
                    result = ExtractSetterParams(contAcc, input.site.leftIsIndex, TemplateBinding{}, table);
                    if (!result.empty())
                    {
                        break;
                    }
                }
            }
        }
    }

    if (result.empty() && !input.leftType.empty())
    {
        ParameterInformation fallback("", std::string(input.leftType));
        fallback.isHandle = IsHandleTypeView(input.leftType);
        fallback.isConst = IsConstTypeView(input.leftType);
        result.push_back(std::move(fallback));
    }
    return result;
}

/**
 * @brief Validates a compound assignment on a property (which evaluates `get_X() op rhs` before calling `set_X`).
 * @param[in] input Assignment site, resolved types, scope, and property metadata.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckPropertyCompoundAssignment(const AssignmentConversionInput& input, DiagnosticContext& ctx)
{
    const std::string_view displayTarget = input.leftType.empty() ? std::string_view("value") : input.leftType;
    if (input.rightType == "void")
    {
        ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {"void", displayTarget});
        return;
    }
    if (input.rightType.empty())
    {
        if (IsAssignmentRhsFunctionReference(input.site, input.scope, ctx))
        {
            ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion",
                            {TrimString(input.site.right.text), displayTarget});
        }
        return;
    }
    if (input.leftType.empty())
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayType = ctx.request.GetEffectiveArrayTypeName();
    const std::string normLeft = UnwrapTypedef(NormalizeType(input.leftType, arrayType), table);
    const std::string normRight = UnwrapTypedef(NormalizeType(input.rightType, arrayType), table);
    if (normLeft == "bool")
    {
        ctx.EmitAtRange(input.site.range, diagnostics::codes::NoMathConversion, normLeft);
        return;
    }

    const std::string_view binOp = CompoundAssignmentBinaryOpName(input.site.op);
    if (ResolvesToEnum(normLeft, table))
    {
        ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {"int", normLeft});
        return;
    }
    if (IsNumericPrimitive(normLeft))
    {
        const bool bitMismatch = IsBitwiseCompoundOp(input.site.op) && IsPrimitiveTypeName(normRight) &&
                                 (!IsIntegerPrimitive(normLeft) || !IsIntegerPrimitive(normRight));
        if (bitMismatch || (!IsNumericPrimitive(normRight) && !ResolvesToEnum(normRight, table) &&
                            !IsConvertible(normRight, normLeft, ctx)))
        {
            ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {normRight, normLeft});
        }
        return;
    }

    std::string owner = MemberOwnerType(normLeft, arrayType);
    const std::string_view ownerView = owner.empty() ? std::string_view(normLeft) : std::string_view(owner);
    if (IsKnownType(ownerView, table, ctx.request.GetEffectiveStringTypeName()) &&
        !ctx.request.IsRegisteredSymbol(ownerView) && HierarchyIsFullyVisible(ownerView, table) && !binOp.empty())
    {
        const bool isStringAdd =
            (binOp == "opAdd" && IsStringType(normLeft, ctx) &&
             (IsPrimitiveTypeName(normRight) || ResolvesToEnum(normRight, table) || IsStringType(normRight, ctx)));
        if (!isStringAdd && !TypeHasOperator(ownerView, binOp, normRight, table))
        {
            ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {normRight, normLeft});
        }
    }
}

void CheckPropertySetterAssignment(const AssignmentConversionInput& input, DiagnosticContext& ctx)
{
    if (!input.propertyInfo.hasSet && input.propertyInfo.hasGet)
    {
        return;
    }
    if (!input.site.opIsAssign && input.site.op != "=")
    {
        CheckPropertyCompoundAssignment(input, ctx);
        return;
    }

    const std::vector<ParameterInformation> targetParams = CollectPropertySetterParams(input, ctx);
    if (targetParams.empty())
    {
        return;
    }

    std::string displayTo = CleanExpressionType(targetParams.front().typeName);
    if (displayTo.empty())
    {
        displayTo = targetParams.front().typeName;
    }

    if (input.rightType == "void")
    {
        ctx.EmitAtRange(input.site.range, "as-err-no-implicit-conversion", {"void", displayTo});
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    bool anyFuncdefTarget = false;
    for (const auto& param : targetParams)
    {
        if (const auto fd = FindFuncdef(CleanBaseTypeView(param.typeName), table))
        {
            anyFuncdefTarget = true;
            if (input.site.rightFuncdef.present)
            {
                CheckFuncdefAssignment(input.site.range, fd->GetFuncdef(), input.site.rightFuncdef, ctx);
            }
        }
    }

    if (input.rightType.empty())
    {
        if (!anyFuncdefTarget && IsAssignmentRhsFunctionReference(input.site, input.scope, ctx))
        {
            ctx.EmitAtRange(input.site.range, "as-err-no-implicit-conversion",
                            {TrimString(input.site.right.text), displayTo});
        }
        return;
    }

    const ExpressionType rightVal = ResolveValueType(input.site.rightValue, input.scope, ctx);
    for (const auto& param : targetParams)
    {
        const bool paramIsHandle = param.isHandle || IsHandleTypeView(param.typeName);
        const bool paramIsConst = param.isConst || IsConstTypeView(param.typeName);
        if (paramIsHandle && !paramIsConst && rightVal.isConst && input.rightType != "null")
        {
            continue;
        }
        if (IsAssignmentValueCompatibleWithParam(input.rightType, param, ctx))
        {
            CheckFloatTruncation(input.site.right, {CleanBaseType(input.rightType), CleanBaseType(param.typeName)},
                                 input.scope, ctx);
            return;
        }
    }

    std::string displayFrom = CleanExpressionType(input.rightType);
    if (displayFrom.empty())
    {
        displayFrom = std::string(input.rightType);
    }
    ctx.EmitAtRange(input.site.range, "as-err-no-implicit-conversion", {displayFrom, displayTo});
}

} // namespace angel_lsp::analysis
