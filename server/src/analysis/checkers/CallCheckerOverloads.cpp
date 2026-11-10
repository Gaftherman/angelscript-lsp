#include "analysis/checkers/CallCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/resolution/AmbiguityDetector.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

static size_t ResolveParameterIndexForArgument(const FunctionSignature& sig, size_t argIdx, std::string_view argName,
                                               const ankerl::unordered_dense::set<size_t>& matchedParams)
{
    if (!argName.empty())
    {
        for (size_t p = 0; p < sig.parameters.size(); ++p)
        {
            if (sig.parameters[p].name == argName)
            {
                return matchedParams.contains(p) ? size_t(-1) : p;
            }
        }
        return size_t(-1);
    }

    if (argIdx >= sig.parameters.size())
    {
        return InspectFunctionArity(sig).isVariadic ? argIdx : size_t(-1);
    }
    return matchedParams.contains(argIdx) ? size_t(-1) : argIdx;
}

static bool CheckUnmatchedDefaultParameters(const FunctionSignature& sig,
                                            const ankerl::unordered_dense::set<size_t>& matchedParams)
{
    for (size_t p = 0; p < sig.parameters.size(); ++p)
    {
        if (!matchedParams.contains(p) && sig.parameters[p].defaultValue.empty())
        {
            return false;
        }
    }
    return true;
}

static bool CheckCandidateNamedArgs(const FunctionSignature& sig, const std::vector<std::string>& argNames,
                                    const std::vector<std::string>& argTypes, const SymbolTable& symbolTable)
{
    ankerl::unordered_dense::set<size_t> matchedParams;
    for (size_t i = 0; i < argNames.size(); ++i)
    {
        size_t paramIdx = ResolveParameterIndexForArgument(sig, i, argNames[i], matchedParams);
        if (paramIdx == size_t(-1))
        {
            return false;
        }
        if (paramIdx < sig.parameters.size())
        {
            matchedParams.insert(paramIdx);
            if (!argTypes[i].empty())
            {
                const auto conv = EvaluateArgumentConversion(argTypes[i], sig.parameters[paramIdx], symbolTable, true);
                if (!conv.IsViable())
                {
                    return false;
                }
            }
        }
    }

    return CheckUnmatchedDefaultParameters(sig, matchedParams);
}

static TSNode GetBoundCallName(TSNode callee)
{
    const auto& gs = parser::GetGrammarSymbols();
    const auto symbol = ts_node_symbol(callee);
    if (symbol == gs.symMemberExpression)
    {
        return parser::GetChildByField(callee, gs.fieldMember);
    }
    if (symbol == gs.symScopedIdentifier)
    {
        TSNode name{};
        parser::ForEachNamedChildNode(callee,
                                      [&](TSNode child)
                                      {
                                          if (ts_node_symbol(child) == gs.symIdentifier)
                                          {
                                              name = child;
                                          }
                                      });
        return name;
    }
    return symbol == gs.symIdentifier ? callee : TSNode{};
}

static void RecordBoundCall(const Symbol& declaration, const CallValidationContext& valCtx)
{
    if (!valCtx.ctx.request.mutableScopeRoot || declaration.type != SymbolType::Function || declaration.isSynthesized ||
        declaration.fileUri.empty() || declaration.selectionRange == SourceRange{})
    {
        return;
    }
    const TSNode name = GetBoundCallName(valCtx.callee);
    if (ts_node_is_null(name) || parser::NodeText(name, valCtx.request.sourceCode) != declaration.name)
    {
        return;
    }
    auto& bindings = valCtx.ctx.callBindings;
    if (!bindings)
    {
        const auto& request = valCtx.ctx.request;
        bindings = std::make_shared<CallBindings>(request.callBindingRevision,
                                                  request.moduleContext ? request.moduleContext->name : std::string{});
    }
    const TSPoint start = ts_node_start_point(name);
    const TSPoint end = ts_node_end_point(name);
    bindings->Add(ts_node_start_byte(name), {start.row, start.column, end.row, end.column}, declaration);
}

static void HandleNamedCallOverloads(std::span<const Symbol* const> matchingArity, const CallResolutionContext& args,
                                     const CalleeResolution& calleeRes, const CallValidationContext& valCtx)
{
    const Symbol* matched = nullptr;
    for (const Symbol* cand : matchingArity)
    {
        if (cand && std::holds_alternative<FunctionSignature>(cand->signature) &&
            CheckCandidateNamedArgs(cand->GetFunction(), args.argNames, args.argTypes, valCtx.ctx.request.symbolTable))
        {
            if (matched)
            {
                return;
            }
            matched = cand;
        }
    }
    if (!args.allArgsResolved)
    {
        return;
    }
    if (matched)
    {
        RecordBoundCall(*matched, valCtx);
    }
    else
    {
        valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), "as-err-call-no-matching-signature",
                               calleeRes.reportedName);
    }
}

static void ValidateResolvedCall(const OverloadMatchResult& match, size_t candidateCount,
                                 const CallResolutionContext& args, const CallValidationContext& valCtx)
{
    const auto& selected = *match.bestCandidate;
    if (!match.isAmbiguous && args.allArgsResolved)
    {
        RecordBoundCall(selected, valCtx);
    }
    ValidateOutArguments(selected, args.argNodes, valCtx);
    if (candidateCount > 1)
    {
        CheckInitializerListArgs(args, selected.GetFunction(), valCtx);
    }
}

void CheckCallOverloads(std::span<const Symbol* const> matchingArity, const CallResolutionContext& args,
                        const CalleeResolution& calleeRes, const CallValidationContext& valCtx)
{
    if (matchingArity.empty())
    {
        return;
    }
    if (args.argTypes.empty() && !calleeRes.candidatesAreFreeFunctions)
    {
        if (matchingArity.size() == 1)
        {
            RecordBoundCall(*matchingArity.front(), valCtx);
        }
        return;
    }

    const bool hasNamedArgs =
        std::any_of(args.argNames.begin(), args.argNames.end(), [](const std::string& n) { return !n.empty(); });
    if (hasNamedArgs)
    {
        HandleNamedCallOverloads(matchingArity, args, calleeRes, valCtx);
        return;
    }

    OverloadMatchResult match =
        ResolveBestOverload(matchingArity, args.argTypes, valCtx.ctx.request.symbolTable, args.argIsLValue);
    if (match.isAmbiguous && args.allArgsResolved)
    {
        const std::string_view prioritySig =
            match.priorityCandidate != nullptr ? match.priorityCandidate->GetDisplaySignature() : std::string_view{};
        const bool hasInitList =
            std::any_of(args.argTypes.begin(), args.argTypes.end(),
                        [](const std::string& t) { return t == constants::k_initializerListTypeSentinel; }) ||
            std::any_of(args.argNodes.begin(), args.argNodes.end(),
                        [](TSNode n)
                        {
                            TSNode unwrapped = UnwrapParenNode(n);
                            return !ts_node_is_null(unwrapped) &&
                                   ts_node_symbol(unwrapped) == parser::GetGrammarSymbols().symInitializerList;
                        });
        valCtx.ctx.EmitAtRange(GetCalleeDiagnosticRange(valCtx.callee), "as-err-call-ambiguous",
                               {std::string_view(calleeRes.reportedName), prioritySig},
                               hasInitList ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning);
    }
    else if (match.viableCandidates.empty() || match.bestCandidate == nullptr)
    {
        ReportOverloadResolutionFailure(matchingArity, args, calleeRes.reportedName, valCtx);
    }
    else
    {
        ValidateResolvedCall(match, matchingArity.size(), args, valCtx);
    }
}

} // namespace angel_lsp::analysis
