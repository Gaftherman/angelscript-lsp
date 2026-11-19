#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace angel_lsp::analysis
{

std::string_view CompoundAssignmentBinaryOpName(std::string_view op) noexcept
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 12> k_ops = {{
        {"+=", "opAdd"},
        {"-=", "opSub"},
        {"*=", "opMul"},
        {"/=", "opDiv"},
        {"%=", "opMod"},
        {"**=", "opPow"},
        {"&=", "opAnd"},
        {"|=", "opOr"},
        {"^=", "opXor"},
        {"<<=", "opShl"},
        {">>=", "opShr"},
        {">>>=", "opUShr"},
    }};
    for (const auto& [assignOp, binOp] : k_ops)
    {
        if (assignOp == op)
        {
            return binOp;
        }
    }
    return {};
}

bool IsBitwiseCompoundOp(std::string_view op) noexcept
{
    return op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=" || op == ">>>=";
}

bool IsAssignmentRhsFunctionReference(const AssignmentSiteFact& site, const Scope* scope, const DiagnosticContext& ctx)
{
    const ExpressionTypeContext exprCtx(scope, ctx);
    if (IsFunctionReference(site.right.bytes, exprCtx))
    {
        return true;
    }
    return site.rightIsHandle && IsFunctionReference(site.rightValue.bytes, exprCtx);
}

bool IsAssignmentValueCompatibleWithParam(std::string_view rawArgType, const ParameterInformation& param,
                                          const DiagnosticContext& ctx)
{
    if (parser::primitives::IsVoidType(rawArgType))
    {
        return false;
    }
    // `CleanBaseTypeView` drops `[]` and template arguments, so an array or template parameter would be
    // judged by its element type; only a plain parameter type is exempt as host-registered.
    const bool paramIsComposite = param.typeName.find_first_of("[<") != std::string::npos;
    if (ctx.request.IsRegisteredSymbol(CleanBaseTypeView(rawArgType)) ||
        (!paramIsComposite && ctx.request.IsRegisteredSymbol(CleanBaseTypeView(param.typeName))))
    {
        return true;
    }
    const SymbolTable& table = ctx.request.symbolTable;
    if (EvaluateArgumentConversion(rawArgType, param, table).IsViable())
    {
        return true;
    }
    if (const auto argFd = FindFuncdef(CleanBaseTypeView(rawArgType), table))
    {
        if (const auto paramFd = FindFuncdef(CleanBaseTypeView(param.typeName), table))
        {
            return AreFuncdefSignaturesCompatible(argFd->GetFuncdef(), paramFd->GetFuncdef(), table);
        }
    }
    return false;
}

/**
 * @brief Checks whether a class or template type declares or inherits an assignment operator accepting rawRightType.
 * @param[in] targetType Normalized target type spelling.
 * @param[in] opName Operator method name such as `opAssign` or `opAddAssign`.
 * @param[in] rawRightType Resolved RHS type spelling.
 * @param[in] ctx Diagnostic context.
 * @return True when a matching non-deleted operator overload accepts the RHS type.
 */
static bool HasMatchingAssignmentOperator(std::string_view targetType, std::string_view opName,
                                          std::string_view rawRightType, const DiagnosticContext& ctx)
{
    if (targetType.empty() || opName.empty())
    {
        return false;
    }
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayType = ctx.request.GetEffectiveArrayTypeName();
    std::string owner = MemberOwnerType(targetType, arrayType);
    if (owner.empty())
    {
        owner = std::string(targetType);
    }
    const TemplateBinding binding =
        (targetType.find('<') != std::string_view::npos) ? BindTemplateArguments(targetType, table) : TemplateBinding{};

    for (const auto& cls : GetInheritedTypeHierarchy(owner, table))
    {
        const auto symbols = table.FindMemberSymbolPtr(cls, opName);
        if (!symbols)
        {
            continue;
        }
        for (const auto& sym : *symbols)
        {
            if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
            {
                continue;
            }
            const auto& fn = sym.GetFunction();
            if (fn.modifiers.isDelete || !AcceptsSingleArgument(fn.parameters))
            {
                continue;
            }
            ParameterInformation param = fn.parameters.front();
            if (!sym.containerName.empty())
            {
                param.typeName = QualifyTypeInContainer(param.typeName, sym.containerName, table);
            }
            if (binding.usable)
            {
                param.typeName = SubstituteTemplateParameters(param.typeName, binding);
            }
            if (IsAssignmentValueCompatibleWithParam(rawRightType, param, ctx))
            {
                return true;
            }
        }
    }
    return false;
}

void CheckAssignmentOperatorConversion(const AssignmentConversionInput& input, const ConversionTypePair& types,
                                       DiagnosticContext& ctx)
{
    if (types.to.empty())
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string_view arrayType = ctx.request.GetEffectiveArrayTypeName();
    std::string owner = MemberOwnerType(types.to, arrayType);
    const std::string_view ownerView = owner.empty() ? std::string_view(types.to) : std::string_view(owner);

    if (input.site.opIsAssign || input.site.op == "=")
    {
        if (auto opSyms = table.FindMemberSymbolPtr(ownerView, "opAssign"))
        {
            for (const auto& sym : *opSyms)
            {
                if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
                    sym.GetFunction().modifiers.isDelete)
                {
                    ctx.EmitAtRange(input.site.range, "as-err-deleted-method-called", {types.to, "opAssign"});
                    break;
                }
            }
        }
    }

    const std::string_view overload =
        input.site.op.empty() ? std::string_view() : AssignmentOverloadName(input.site.op);
    const bool isCompound = !input.site.opIsAssign && input.site.op != "=" && !overload.empty();
    if (isCompound && types.to == "bool")
    {
        ctx.EmitAtRange(input.site.range, diagnostics::codes::NoMathConversion, types.to);
        return;
    }

    if (types.from.empty())
    {
        return;
    }

    if (HasMatchingAssignmentOperator(types.to, overload, input.rightType, ctx))
    {
        return;
    }

    if (isCompound)
    {
        if (ResolvesToEnum(types.to, table))
        {
            ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {"int", types.to});
            return;
        }
        if (IsPrimitiveTypeName(types.to))
        {
            const bool bitMismatch = IsBitwiseCompoundOp(input.site.op) && IsPrimitiveTypeName(types.from) &&
                                     (!IsIntegerPrimitive(types.to) || !IsIntegerPrimitive(types.from));
            if (bitMismatch || !IsConvertible(types.from, types.to, ctx))
            {
                ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {types.from, types.to});
            }
            return;
        }
        if (!ctx.request.IsRegisteredSymbol(ownerView) && FindTypeDeclaration(ownerView, table).found &&
            HierarchyIsFullyVisible(ownerView, table))
        {
            ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {types.from, types.to});
            return;
        }
        if (IsStringType(types.to, ctx) && overload == "opAddAssign")
        {
            if (!IsNumericPrimitive(types.from) && types.from != "bool" && types.from != types.to &&
                !IsConvertible(types.from, types.to, ctx))
            {
                ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {types.from, types.to});
            }
        }
        return;
    }

    if (types.to != types.from && !IsConvertible(types.from, types.to, ctx))
    {
        ctx.EmitAtRange(input.site.right.range, "as-err-no-implicit-conversion", {types.from, types.to});
    }
}

} // namespace angel_lsp::analysis
