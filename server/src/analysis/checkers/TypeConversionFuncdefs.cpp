#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/types/TypeSanitization.h"

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

/**
 * @brief Checks a lambda value against the parameter list of the target funcdef.
 * @param[in] target Diagnostic anchor range.
 * @param[in] funcdefSig Expected funcdef signature.
 * @param[in] value Indexed assigned value.
 * @param[in,out] ctx Diagnostic context.
 * @return True when the value is a lambda, which ends the funcdef check whether or not it conflicts.
 */
static bool CheckLambdaFuncdefAssignment(SourceRange target, const FuncdefSignature& funcdefSig,
                                         const FuncdefValueFact& value, DiagnosticContext& ctx)
{
    if (!value.isLambda)
    {
        return false;
    }
    if (value.hasParameterList && LambdaContradictsFuncdef(value.lambdaParameters, funcdefSig, ctx.request.symbolTable))
    {
        ctx.EmitAtRange(target, "as-err-signature-mismatch-func-handle");
    }
    return true;
}

/**
 * @brief Checks a named function value against the target funcdef signature.
 * @param[in] target Diagnostic anchor range.
 * @param[in] funcdefSig Expected funcdef signature.
 * @param[in] value Indexed assigned value naming a function.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckNamedFunctionFuncdefAssignment(SourceRange target, const FuncdefSignature& funcdefSig,
                                                const FuncdefValueFact& value, DiagnosticContext& ctx)
{
    if (value.text.empty() || value.text == "null")
    {
        return;
    }

    const std::vector<Symbol> candidates = CollectFunctionCandidates(value.text, ctx.request.symbolTable);
    if (candidates.empty())
    {
        return;
    }

    if (!HasMatchingFuncdefCandidate(candidates, funcdefSig))
    {
        ctx.EmitAtRange(target, "as-err-signature-mismatch-func-handle");
    }
}

void CheckFuncdefAssignment(SourceRange target, const FuncdefSignature& funcdefSig, const FuncdefValueFact& value,
                            DiagnosticContext& ctx)
{
    if (!value.present)
    {
        return;
    }
    if (CheckLambdaFuncdefAssignment(target, funcdefSig, value, ctx))
    {
        return;
    }
    CheckNamedFunctionFuncdefAssignment(target, funcdefSig, value, ctx);
}

} // namespace angel_lsp::analysis
