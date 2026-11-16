#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ExpressionSiteFacts.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <string>
#include <string_view>
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

void CheckDelegateConstruction(const CallConstructSiteFact& site, const FuncdefSignature& funcdefSig,
                               const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    if (!site.argument.present || site.argument.value.kind != ValueExpressionKind::Member || !request.expressionSites)
    {
        return;
    }

    const auto* memFact = request.expressionSites->FindMember(site.argument.value.bytes);
    if (!memFact || memFact->propertyAccess.kind != PropertyAccessKind::Member)
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(site.range, request);
    const std::string rawObj =
        ResolveExpressionType(memFact->propertyAccess.receiver, ExpressionTypeContext(scope, ctx));
    const SymbolTable& table = ctx.request.symbolTable;
    const std::string cleanObj = UnwrapTypedef(CleanBaseType(rawObj), table);
    if (cleanObj.empty() || cleanObj == "auto" || cleanObj == "?" || cleanObj == "any" ||
        cleanObj.find('<') != std::string::npos || ctx.request.IsRegisteredSymbol(cleanObj) ||
        !HierarchyIsFullyVisible(cleanObj, table))
    {
        return;
    }

    std::vector<Symbol> candidates;
    for (const auto& ancestor : GetInheritedTypeHierarchy(cleanObj, table))
    {
        if (const auto syms = table.FindSymbolsPtr(ancestor))
        {
            for (const auto& s : *syms)
            {
                if ((!ctx.request.predefinedFileExtension.empty() &&
                     s.fileUri.ends_with(ctx.request.predefinedFileExtension)) ||
                    (s.type == SymbolType::Class && s.GetClass().isTemplate))
                {
                    return;
                }
            }
        }
        if (const auto members = table.FindMemberSymbolPtr(ancestor, memFact->propertyAccess.text))
        {
            for (const auto& m : *members)
            {
                if (m.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(m.signature))
                {
                    candidates.push_back(m);
                }
            }
        }
    }

    if (!candidates.empty() && !HasMatchingFuncdefCandidate(candidates, funcdefSig))
    {
        ctx.EmitAtRange(site.range, diagnostics::codes::CannotCreateDelegate);
    }
}

} // namespace angel_lsp::analysis
