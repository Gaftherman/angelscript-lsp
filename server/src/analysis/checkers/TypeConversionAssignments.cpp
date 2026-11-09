#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Rejects assigning a const handle target to a non-const one when both sides are `@` expressions.
 * @param[in] site Indexed assignment.
 * @param[in] types Cleaned source and target types of the assignment.
 * @param[in] scope Lexical scope the assignment lives in.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckHandleConstAssignment(const AssignmentSiteFact& site, const ConversionTypePair& types,
                                       const Scope* scope, DiagnosticContext& ctx)
{
    if (!site.leftIsHandle && !site.rightIsHandle)
    {
        return;
    }

    const ExpressionType leftVal = ResolveValueType(site.leftValue, scope, ctx);
    const ExpressionType rightVal = ResolveValueType(site.rightValue, scope, ctx);
    if (rightVal.isConst && !leftVal.isConst && !types.from.empty() && !types.to.empty())
    {
        ctx.EmitAtRange(site.right.range, "as-err-no-implicit-conversion",
                        {"const " + types.from + "@", types.to + "@"});
    }
}

/**
 * @brief Reports a deleted opAssign and a missing implicit conversion on the assigned type.
 * @param[in] site Indexed assignment.
 * @param[in] types Cleaned source and target types of the assignment.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckAssignmentOperatorConversion(const AssignmentSiteFact& site, const ConversionTypePair& types,
                                              DiagnosticContext& ctx)
{
    if (types.to.empty())
    {
        return;
    }

    auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(types.to, "opAssign");
    if (opSyms)
    {
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature) &&
                sym.GetFunction().modifiers.isDelete)
            {
                ctx.EmitAtRange(site.range, "as-err-deleted-method-called", {types.to, "opAssign"});
                break;
            }
        }
    }

    if (!types.from.empty() && types.to != types.from)
    {
        const std::string_view overload = site.op.empty() ? std::string_view() : AssignmentOverloadName(site.op);
        const bool operatorHandlesIt = DeclaresOperatorMethod(types.to, overload, ctx.request.symbolTable);
        if (!operatorHandlesIt && !IsConvertible(types.from, types.to, ctx))
        {
            ctx.EmitAtRange(site.right.range, "as-err-no-implicit-conversion", {types.from, types.to});
        }
    }
}

/**
 * @brief Reports whether a type accepts a handle through opAssign or opHndlAssign.
 * @param[in] clean Type name without decorations.
 * @param[in] table Symbol table searched for members.
 * @return True when a handle-taking assignment operator exists.
 */
static bool HasHandleOpAssign(std::string_view clean, const SymbolTable& table)
{
    if (auto opSyms = table.FindMemberSymbolPtr(clean, "opAssign"))
    {
        for (const auto& sym : *opSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (!fn.parameters.empty() &&
                    (fn.parameters[0].isHandle || IsHandleTypeView(fn.parameters[0].typeName)))
                {
                    return true;
                }
            }
        }
    }
    return table.FindMemberSymbolPtr(clean, "opHndlAssign") != nullptr;
}

/**
 * @brief Reports whether a type has an implicit single-handle constructor.
 * @param[in] clean Type name without decorations.
 * @param[in] table Symbol table searched for members.
 * @return True when a non-explicit constructor takes one handle parameter.
 */
static bool HasHandleConvertingCtor(std::string_view clean, const SymbolTable& table)
{
    if (auto ctorSyms = table.FindMemberSymbolPtr(clean, clean))
    {
        for (const auto& sym : *ctorSyms)
        {
            if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
            {
                const auto& fn = sym.GetFunction();
                if (!fn.modifiers.isExplicit && fn.parameters.size() == 1 &&
                    (fn.parameters[0].isHandle || IsHandleTypeView(fn.parameters[0].typeName)))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Reports whether a null literal can be assigned to a value of the type.
 * @param[in] typeName Type name, possibly decorated.
 * @param[in] table Symbol table searched for members.
 * @return True for a non-primitive type with a handle-taking assignment operator or converting constructor.
 */
static bool TypeAcceptsNullHandle(std::string_view typeName, const SymbolTable& table)
{
    const std::string_view clean = CleanBaseTypeView(typeName);
    if (clean.empty() || IsCorePrimitive(clean))
    {
        return false;
    }
    return HasHandleOpAssign(clean, table) || HasHandleConvertingCtor(clean, table);
}

/**
 * @brief Checks `= null` against the type of the assigned left side.
 * @param[in] site Indexed assignment.
 * @param[in] rawLeftType Resolved left type, uncleaned.
 * @param[in,out] ctx Diagnostic context.
 * @return True when the right side is a null assignment, which ends the remaining assignment checks.
 */
static bool CheckNullAssignmentToNonHandle(const AssignmentSiteFact& site, std::string_view rawLeftType,
                                           DiagnosticContext& ctx)
{
    if (!site.rightIsNull)
    {
        return false;
    }

    const std::string_view opText = site.op.empty() ? std::string_view("=") : site.op;
    if (opText != "=")
    {
        return false;
    }

    if (rawLeftType.empty() || ExpressionTypeCache::IsPoisonType(rawLeftType))
    {
        return false;
    }

    if (rawLeftType == "auto" || rawLeftType == "?" || rawLeftType == "any")
    {
        return true;
    }

    if (IsHandleTypeView(rawLeftType) && site.leftIsHandle)
    {
        return true;
    }

    if (!TypeAcceptsNullHandle(rawLeftType, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(site.right.range, "as-err-null-non-handle", rawLeftType);
    }
    return true;
}

/**
 * @brief Reports a right side that spells only a namespace name.
 * @param[in] right Right operand of the assignment.
 * @param[in,out] ctx Diagnostic context.
 * @return True when every symbol of that name is a namespace, which ends the remaining checks.
 */
static bool CheckRightSideNamespaceOnly(const BinaryOperandFact& right, DiagnosticContext& ctx)
{
    const std::string_view rightIdent = TrimString(right.text);
    if (rightIdent.empty())
    {
        return false;
    }
    if (auto syms = ctx.request.symbolTable.FindSymbolsPtr(rightIdent))
    {
        for (const auto& s : *syms)
        {
            if (s.type != SymbolType::Namespace)
            {
                return false;
            }
        }
        ctx.EmitAtRange(right.range, "as-err-undefined-identifier", rightIdent);
        return true;
    }
    return false;
}

void ProcessAssignmentSite(const AssignmentSiteFact& site, const TypeConversionCheckRequest& request,
                           DiagnosticContext& ctx)
{
    const Scope* scope = ResolveNodeScope(site.range, request);
    if (const auto dt = IsBareDataType(site.left.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(ToConversionExpression(site.left), ctx, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(site.right.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(ToConversionExpression(site.right), ctx, *dt);
        return;
    }

    if (CheckRightSideNamespaceOnly(site.right, ctx))
    {
        return;
    }

    CheckAssignmentPropertyAccess(site, scope, ctx);

    const auto leftType = ResolveExpressionType(site.left.bytes, ExpressionTypeContext(scope, ctx));
    if (CheckNullAssignmentToNonHandle(site, leftType, ctx))
    {
        return;
    }

    const auto rightType = ResolveExpressionType(site.right.bytes, ExpressionTypeContext(scope, ctx));
    if (ExpressionTypeCache::IsPoisonType(leftType) || ExpressionTypeCache::IsPoisonType(rightType))
    {
        return;
    }
    const ConversionTypePair types{CleanBaseType(rightType), CleanBaseType(leftType)};

    CheckFloatTruncation(site.right, types, scope, ctx);

    auto leftFuncdef = FindFuncdef(types.to, ctx.request.symbolTable);
    if (leftFuncdef)
    {
        CheckFuncdefAssignment(site.range, leftFuncdef->GetFuncdef(), site.rightFuncdef, ctx);
    }
    else if (types.from.empty() && IsFunctionReference(site.right.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(site.right.range, "as-err-no-implicit-conversion", {TrimString(site.right.text), types.to});
    }

    CheckHandleConstAssignment(site, types, scope, ctx);
    CheckAssignmentOperatorConversion(site, types, ctx);
}

} // namespace angel_lsp::analysis
