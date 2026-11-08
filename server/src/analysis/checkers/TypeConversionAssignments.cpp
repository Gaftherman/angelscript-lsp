#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"

#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

static TSNode UnwrapHandleOperand(TSNode expr, bool& isHandleAssignment, const NodeIndex* index)
{
    const SyntaxOperands syntax(expr, index);
    const auto& gs = parser::GetGrammarSymbols();
    if (syntax.HasUnaryOperator(gs.tokAt))
    {
        isHandleAssignment = true;
        const TSNode operand = syntax.Field(gs.fieldOperand);
        if (!ts_node_is_null(operand))
        {
            return operand;
        }
    }
    return expr;
}

static void CheckHandleConstAssignment(AssignmentOperands operands, const ConversionTypePair& types, const Scope* scope,
                                       DiagnosticContext& ctx)
{
    bool isHandleAssignment = false;
    TSNode actualLeft = UnwrapHandleOperand(operands.left, isHandleAssignment, ctx.request.nodeIndex);
    TSNode actualRight = UnwrapHandleOperand(operands.right, isHandleAssignment, ctx.request.nodeIndex);

    if (!isHandleAssignment)
    {
        return;
    }

    ExpressionType leftVal = ResolveValueType(actualLeft, scope, ctx);
    ExpressionType rightVal = ResolveValueType(actualRight, scope, ctx);
    const bool rightConstTarget = rightVal.isConst;
    const bool leftConstTarget = leftVal.isConst;
    if (rightConstTarget && !leftConstTarget && !types.from.empty() && !types.to.empty())
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(operands.right), "as-err-no-implicit-conversion",
                            {"const " + types.from + "@", types.to + "@"});
    }
}

static void CheckAssignmentOperatorConversion(TSNode node, TSNode right, const ConversionTypePair& types,
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
                ctx.EmitAtNodeRange(ToOptionalSourceRange(node), "as-err-deleted-method-called",
                                    {types.to, "opAssign"});
                break;
            }
        }
    }

    if (!types.from.empty() && types.to != types.from)
    {
        const TSNode assignOp =
            SyntaxOperands(node, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldOperator);
        const std::string_view overload =
            ts_node_is_null(assignOp) ? std::string_view()
                                      : AssignmentOverloadName(parser::NodeText(assignOp, ctx.request.sourceCode));

        const bool operatorHandlesIt = DeclaresOperatorMethod(types.to, overload, ctx.request.symbolTable);
        if (!operatorHandlesIt && !IsConvertible(types.from, types.to, ctx))
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(right), "as-err-no-implicit-conversion", {types.from, types.to});
        }
    }
}

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

static bool TypeAcceptsNullHandle(std::string_view typeName, const SymbolTable& table)
{
    const std::string_view clean = CleanBaseTypeView(typeName);
    if (clean.empty() || IsCorePrimitive(clean))
    {
        return false;
    }
    return HasHandleOpAssign(clean, table) || HasHandleConvertingCtor(clean, table);
}

static bool CheckNullAssignmentToNonHandle(const SyntaxOperands& assignment, AssignmentOperands operands,
                                           const std::string& rawLeftType, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode unwrappedRight = UnwrapIndexedParentheses(operands.right, ctx.request.nodeIndex);
    if (ts_node_symbol(unwrappedRight) != gs.symNullLiteral)
    {
        return false;
    }

    const TSNode assignOp = assignment.Field(gs.fieldOperator);
    const std::string_view opText = ts_node_is_null(assignOp)
                                        ? std::string_view("=")
                                        : std::string_view(parser::NodeText(assignOp, ctx.request.sourceCode));
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

    if (IsHandleTypeView(rawLeftType) &&
        SyntaxOperands(operands.left, ctx.request.nodeIndex).HasUnaryOperator(gs.tokAt))
    {
        return true;
    }

    if (!TypeAcceptsNullHandle(rawLeftType, ctx.request.symbolTable))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(operands.right), "as-err-null-non-handle", rawLeftType);
    }
    return true;
}

static bool CheckRightSideNamespaceOnly(TSNode right, DiagnosticContext& ctx)
{
    const std::string rightIdent = TrimString(parser::NodeText(right, ctx.request.sourceCode));
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
        ctx.EmitAtNodeRange(ToOptionalSourceRange(right), "as-err-undefined-identifier", rightIdent);
        return true;
    }
    return false;
}

void ProcessAssignmentNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(node, ctx.request.nodeIndex);
    TSNode left = syntax.Field(parser::GetGrammarSymbols().fieldLeft);
    TSNode right = syntax.Field(parser::GetGrammarSymbols().fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    if (const auto dt = IsBareDataType(left, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(left, ctx, *dt);
        return;
    }
    if (const auto dt = IsBareDataType(right, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(right, ctx, *dt);
        return;
    }

    if (CheckRightSideNamespaceOnly(right, ctx))
    {
        return;
    }

    CheckAssignmentPropertyAccess(node, scope, ctx);

    const std::string leftType = ResolveExpressionType(left, ExpressionTypeContext(scope, ctx));
    if (CheckNullAssignmentToNonHandle(syntax, {left, right}, leftType, ctx))
    {
        return;
    }

    const std::string rightType = ResolveExpressionType(right, ExpressionTypeContext(scope, ctx));
    if (ExpressionTypeCache::IsPoisonType(leftType) || ExpressionTypeCache::IsPoisonType(rightType))
    {
        return;
    }
    const ConversionTypePair types{CleanBaseType(rightType), CleanBaseType(leftType)};

    CheckFloatTruncation(right, types, scope, ctx);

    auto leftFuncdef = FindFuncdef(types.to, ctx.request.symbolTable);
    if (leftFuncdef)
    {
        CheckFuncdefAssignment(node, leftFuncdef->GetFuncdef(), right, ctx);
    }
    else if (types.from.empty() && IsFunctionReference(right, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(right), "as-err-no-implicit-conversion",
                            {TrimString(parser::NodeText(right, ctx.request.sourceCode)), types.to});
    }

    CheckHandleConstAssignment({left, right}, types, scope, ctx);
    CheckAssignmentOperatorConversion(node, right, types, ctx);
}

} // namespace angel_lsp::analysis
