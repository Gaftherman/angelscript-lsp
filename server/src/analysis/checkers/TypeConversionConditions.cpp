#include "analysis/checkers/RepeatedConversionChecker.h"
#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool IsLogicalBinaryOperator(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return sym == gs.tokAmpAmp || sym == gs.tokAnd || sym == gs.tokPipePipe || sym == gs.tokOr ||
           sym == gs.tokCaretCaret || sym == gs.tokXor;
}

static bool IsLogicalUnaryOperator(TSSymbol sym, const parser::GrammarSymbols& gs)
{
    return sym == gs.tokBang || sym == gs.tokNot;
}

static void CollectBooleanOperands(TSNode rootExpr, const NodeIndex* index, std::vector<TSNode>& operands)
{
    if (ts_node_is_null(rootExpr))
    {
        return;
    }

    std::vector<TSNode> worklist{rootExpr};
    const auto& gs = parser::GetGrammarSymbols();

    while (!worklist.empty())
    {
        TSNode expr = worklist.back();
        worklist.pop_back();

        if (ts_node_is_null(expr))
        {
            continue;
        }

        const TSSymbol sym = ts_node_symbol(expr);

        const SyntaxOperands exprOperands(expr, index);
        if (sym == gs.symBinaryExpression)
        {
            const TSNode op = exprOperands.Field(gs.fieldOperator);
            const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);
            if (IsLogicalBinaryOperator(opSym, gs))
            {
                worklist.push_back(exprOperands.Field(gs.fieldRight));
                worklist.push_back(exprOperands.Field(gs.fieldLeft));
                continue;
            }
        }
        else if (sym == gs.symUnaryExpression)
        {
            const TSNode op = exprOperands.Field(gs.fieldOperator);
            const TSSymbol opSym = ts_node_is_null(op) ? 0 : ts_node_symbol(op);
            if (IsLogicalUnaryOperator(opSym, gs))
            {
                worklist.push_back(exprOperands.Field(gs.fieldOperand));
                continue;
            }
        }

        operands.push_back(expr);
    }
}

const Scope* ResolveNodeScope(TSNode node, const TypeConversionCheckRequest& request)
{
    const TSPoint start = ts_node_start_point(node);
    return FindInnermostScope(request.scopeRoot, start.row, start.column);
}

static TSNode ExtractConditionNode(TSNode node, TSSymbol sym, const NodeIndex* index)
{
    const auto& gs = parser::GetGrammarSymbols();
    const SyntaxOperands syntax(node, index);
    TSNode condition = syntax.Field(gs.fieldCondition);
    if (!ts_node_is_null(condition) && ts_node_symbol(condition) == gs.symExpressionStatement)
    {
        condition = SyntaxOperands(condition, index).FirstNamed();
    }
    if (ts_node_is_null(condition))
    {
        condition = syntax.NamedAt(sym == gs.symDoWhileStatement ? 1 : 0);
    }
    return condition;
}

static void ValidateConditionOperand(TSNode operand, const std::string& operandType, DiagnosticContext& ctx)
{
    const bool isKnown = IsKnownType(operandType, ctx);

    if (isKnown && !IsTruthyCondition(operandType, ctx.request.symbolTable))
    {
        const TSPoint start = ts_node_start_point(operand);
        const TSPoint end = ts_node_end_point(operand);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-condition-not-boolean", operandType);
        return;
    }

    if (!ctx.request.diagnostics || !ctx.request.diagnostics->reportBoolConversion ||
        ctx.request.BoolConversionMode() != 0)
    {
        return;
    }

    if (const std::string* conversion = BoolConversionOperator(operandType, ctx.request.symbolTable))
    {
        const TSPoint start = ts_node_start_point(operand);
        const TSPoint end = ts_node_end_point(operand);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-hint-bool-conversion",
                        {operandType, *conversion}, DiagnosticSeverity::Hint);
    }
}

static void CheckBooleanOperands(TSNode condition, const Scope* scope, DiagnosticContext& ctx)
{
    std::vector<TSNode> operands;
    CollectBooleanOperands(condition, ctx.request.nodeIndex, operands);

    for (const TSNode& operand : operands)
    {
        const std::string operandType =
            CleanBaseType(ResolveExpressionType(operand, ExpressionTypeContext(scope, ctx)));

        if (operandType.empty() || operandType == "auto" || operandType == "void")
        {
            continue;
        }

        ValidateConditionOperand(operand, operandType, ctx);
    }
}

static void CheckRefTypeBoolConversion(TSNode condNode, const Scope* scope, DiagnosticContext& ctx)
{
    const TSNode current = UnwrapIndexedParentheses(condNode, ctx.request.nodeIndex);

    if (ts_node_is_null(current))
    {
        return;
    }

    bool isHandleTarget = false;
    if (scope)
    {
        const std::string name = parser::NodeText(current, ctx.request.sourceCode);
        const LocalDefinition* def = ResolveInScopeAt(scope, name, ts_node_start_byte(current));
        if (def)
        {
            isHandleTarget = def->isHandleType || IsHandleTypeView(def->typeName);
        }
    }

    const std::string condType = ResolveExpressionType(current, ExpressionTypeContext(scope, ctx));
    if (!isHandleTarget && !IsHandleTypeView(condType))
    {
        return;
    }

    const std::string baseClass = CleanBaseType(condType);
    auto opSyms = ctx.request.symbolTable.FindMemberSymbolPtr(baseClass, "opImplConv");
    if (!opSyms)
    {
        return;
    }

    for (const auto& sym : *opSyms)
    {
        if (sym.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(sym.signature))
        {
            if (CleanBaseType(sym.GetFunction().returnType) == "bool")
            {
                ctx.EmitAtNode(current, "as-err-ref-type-bool-conv-disallowed", {baseClass, "bool"});
                break;
            }
        }
    }
}

void ProcessConditionNode(TSNode node, const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    TSNode condition = ExtractConditionNode(node, sym, ctx.request.nodeIndex);
    if (ts_node_is_null(condition))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(node, request);
    CheckBooleanOperands(condition, scope, ctx);

    if (sym != gs.symTernaryExpression)
    {
        CheckRefTypeBoolConversion(condition, scope, ctx);
    }

    if (sym == gs.symIfStatement)
    {
        const auto* fact = request.nodeIndex.FindOperandSyntax(node);
        if (!fact || fact->parentSymbol != gs.symIfStatement)
        {
            CheckRepeatedConversions(node, scope, ctx);
        }
    }
}

} // namespace angel_lsp::analysis
