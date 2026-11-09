#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool MatchesFuncdefSignature(const FunctionSignature& fn, const FuncdefSignature& fd)
{
    if (CleanBaseType(fn.returnType) != CleanBaseType(fd.returnType))
    {
        return false;
    }
    if (fn.parameters.size() != fd.parameters.size())
    {
        return false;
    }
    for (size_t i = 0; i < fn.parameters.size(); ++i)
    {
        if (CleanBaseType(fn.parameters[i].typeName) != CleanBaseType(fd.parameters[i].typeName))
        {
            return false;
        }
        if (fn.parameters[i].modifier != fd.parameters[i].modifier)
        {
            return false;
        }
        if (fn.parameters[i].isReference != fd.parameters[i].isReference)
        {
            return false;
        }
        if (fn.parameters[i].isHandle != fd.parameters[i].isHandle)
        {
            return false;
        }
    }
    return true;
}

static TSNode UnwrapAddressOfOperator(TSNode valueNode, const DiagnosticContext& ctx)
{
    const SyntaxOperands syntax(valueNode, ctx.request.nodeIndex);
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(valueNode) == gs.symUnaryExpression)
    {
        TSNode op = syntax.Field(gs.fieldOperator);
        if (!ts_node_is_null(op) &&
            (ts_node_symbol(op) == gs.tokAt || parser::NodeText(op, ctx.request.sourceCode) == "@"))
        {
            TSNode operand = syntax.Field(gs.fieldOperand);
            if (!ts_node_is_null(operand))
            {
                return operand;
            }
        }
    }
    return valueNode;
}

static bool CheckLambdaFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode actualVal,
                                         DiagnosticContext& ctx)
{
    if (ts_node_symbol(actualVal) != parser::GetGrammarSymbols().symLambdaExpression)
    {
        return false;
    }
    TSNode listNode =
        SyntaxOperands(actualVal, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldParameters);
    if (!ts_node_is_null(listNode) && LambdaContradictsFuncdef(ReadLambdaParameters(listNode, ctx.request.sourceCode),
                                                               funcdefSig, ctx.request.symbolTable))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(targetNode), "as-err-signature-mismatch-func-handle");
    }
    return true;
}

static bool HasMatchingFuncdefCandidate(const std::vector<Symbol>& candidates, const FuncdefSignature& funcdefSig)
{
    for (const auto& cand : candidates)
    {
        if (MatchesFuncdefSignature(cand.GetFunction(), funcdefSig))
        {
            return true;
        }
    }
    return false;
}

static void CheckNamedFunctionFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode actualVal,
                                                DiagnosticContext& ctx)
{
    const std::string funcName = TrimString(std::string(parser::NodeText(actualVal, ctx.request.sourceCode)));
    if (funcName.empty() || funcName == "null")
    {
        return;
    }

    const std::vector<Symbol> candidates = CollectFunctionCandidates(funcName, ctx.request.symbolTable);
    if (candidates.empty())
    {
        return;
    }

    if (!HasMatchingFuncdefCandidate(candidates, funcdefSig))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(targetNode), "as-err-signature-mismatch-func-handle");
    }
}

void CheckFuncdefAssignment(TSNode targetNode, const FuncdefSignature& funcdefSig, TSNode valueNode,
                            DiagnosticContext& ctx)
{
    if (ts_node_is_null(valueNode))
    {
        return;
    }

    TSNode actualVal = UnwrapAddressOfOperator(valueNode, ctx);
    if (CheckLambdaFuncdefAssignment(targetNode, funcdefSig, actualVal, ctx))
    {
        return;
    }

    CheckNamedFunctionFuncdefAssignment(targetNode, funcdefSig, actualVal, ctx);
}

} // namespace angel_lsp::analysis
