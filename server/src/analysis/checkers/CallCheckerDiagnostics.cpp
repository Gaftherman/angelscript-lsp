#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

SourceRange GetCalleeDiagnosticRange(TSNode callee)
{
    if (ts_node_is_null(callee))
    {
        return SourceRange{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    TSNode targetNode = callee;
    if (ts_node_symbol(callee) == gs.symMemberExpression)
    {
        const TSNode member = ts_node_child_by_field_id(callee, gs.fieldMember);
        if (!ts_node_is_null(member))
        {
            targetNode = member;
        }
    }
    const TSPoint start = ts_node_start_point(targetNode);
    const TSPoint end = ts_node_end_point(targetNode);
    return {start.row, start.column, end.row, end.column};
}

static bool IsTernaryBranchCompatible(const std::string& branchType, const std::string& expected)
{
    if (branchType.empty())
    {
        return true;
    }
    if (branchType == expected || IsSameType(branchType, expected))
    {
        return true;
    }
    if (branchType == "null" && IsHandleTypeView(expected))
    {
        return true;
    }
    return false;
}

static std::string ResolveTernaryMismatchType(const std::string& t1, const std::string& t2, const std::string& expected)
{
    if (!t1.empty() && !IsTernaryBranchCompatible(t1, expected))
    {
        return t1;
    }
    if (!t2.empty() && !IsTernaryBranchCompatible(t2, expected))
    {
        return t2;
    }
    return !t1.empty() ? t1 : (!t2.empty() ? t2 : "unknown");
}

void CheckMalformedTernaryArgs(const std::vector<TSNode>& argNodes, const std::vector<std::string>& argTypes,
                               std::span<const Symbol* const> candidates, const CallValidationContext& valCtx)
{
    if (candidates.empty() || !candidates[0])
    {
        return;
    }
    const auto& fn = candidates[0]->GetFunction();
    const size_t limit = std::min({argNodes.size(), argTypes.size(), fn.parameters.size()});

    for (size_t i = 0; i < limit; ++i)
    {
        const auto& gs = parser::GetGrammarSymbols();
        TSNode node = UnwrapParenNode(argNodes[i]);
        if (ts_node_symbol(node) != gs.symTernaryExpression || !argTypes[i].empty())
        {
            continue;
        }
        if (IsWildcardParameter(fn.parameters[i]))
        {
            continue;
        }

        const std::string expected = fn.parameters[i].typeName;
        TSNode consequence = parser::GetChildByField(node, gs.fieldConsequence);
        TSNode alternative = parser::GetChildByField(node, gs.fieldAlternative);
        std::string t1 = ResolveExpressionType(consequence, ExpressionTypeContext(valCtx.scope, valCtx.ctx));
        std::string t2 = ResolveExpressionType(alternative, ExpressionTypeContext(valCtx.scope, valCtx.ctx));

        if (IsTernaryBranchCompatible(t1, expected) && IsTernaryBranchCompatible(t2, expected))
        {
            continue;
        }

        std::string badType = ResolveTernaryMismatchType(t1, t2, expected);
        const TSPoint aStart = ts_node_start_point(argNodes[i]);
        const TSPoint aEnd = ts_node_end_point(argNodes[i]);
        valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                               badType, expected);
    }
}

static size_t FindCandidateBadArgument(const Symbol& candidate, const std::vector<std::string>& argTypes,
                                       const SymbolTable& table)
{
    const auto& fn = candidate.GetFunction();
    for (size_t i = 0; i < argTypes.size() && i < fn.parameters.size(); ++i)
    {
        if (!EvaluateArgumentConversion(argTypes[i], fn.parameters[i], table).IsViable())
        {
            return i;
        }
    }
    return argTypes.size();
}

static std::optional<size_t> FindAgreedBlamedArgument(std::span<const Symbol* const> candidates,
                                                      const std::vector<std::string>& argTypes,
                                                      const SymbolTable& table)
{
    if (candidates.empty())
    {
        return std::nullopt;
    }

    size_t blamed = argTypes.size();
    for (const auto* candidate : candidates)
    {
        if (!candidate)
        {
            continue;
        }
        const size_t firstBad = FindCandidateBadArgument(*candidate, argTypes, table);
        if (firstBad == argTypes.size())
        {
            return std::nullopt;
        }
        if (blamed == argTypes.size())
        {
            blamed = firstBad;
        }
        else if (blamed != firstBad)
        {
            return std::nullopt;
        }
    }
    return blamed < argTypes.size() ? std::optional<size_t>(blamed) : std::nullopt;
}

static std::string FormatCallDescriptionWithNull(const std::string& calleeName,
                                                 const std::vector<std::string>& argTypes)
{
    std::string callDesc = calleeName + "(";
    for (size_t i = 0; i < argTypes.size(); ++i)
    {
        if (i > 0)
        {
            callDesc += ", ";
        }
        callDesc += (argTypes[i] == "null" ? "<null handle>" : argTypes[i]);
    }
    callDesc += ")";
    return callDesc;
}

void ReportOverloadResolutionFailure(std::span<const Symbol* const> candidates, const CallResolutionContext& args,
                                     const std::string& reportedName, const CallValidationContext& valCtx)
{
    const auto blamedArg = FindAgreedBlamedArgument(candidates, args.argTypes, valCtx.ctx.request.symbolTable);
    if (blamedArg && *blamedArg < args.argNodes.size())
    {
        if (args.argTypes[*blamedArg] == "null")
        {
            valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), "as-err-call-no-matching-signature",
                                   FormatCallDescriptionWithNull(reportedName, args.argTypes));
            return;
        }

        const auto& fn = candidates[0]->GetFunction();
        const std::string expected =
            *blamedArg < fn.parameters.size() ? fn.parameters[*blamedArg].typeName : std::string();
        const TSPoint aStart = ts_node_start_point(args.argNodes[*blamedArg]);
        const TSPoint aEnd = ts_node_end_point(args.argNodes[*blamedArg]);
        valCtx.ctx.EmitAtRange({aStart.row, aStart.column, aEnd.row, aEnd.column}, "as-err-no-implicit-conversion",
                               args.argTypes[*blamedArg], expected);
        return;
    }

    if (args.allArgsResolved)
    {
        const bool hasNull =
            std::any_of(args.argTypes.begin(), args.argTypes.end(), [](const std::string& t) { return t == "null"; });
        valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), "as-err-call-no-matching-signature",
                               hasNull ? FormatCallDescriptionWithNull(reportedName, args.argTypes) : reportedName);
    }
}

} // namespace angel_lsp::analysis
