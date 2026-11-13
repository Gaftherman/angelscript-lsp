#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/DefiniteAssignmentFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

void DefiniteAssignmentVisitor::CollectCallCandidates(const DefiniteAssignmentCallFact& call,
                                                      std::vector<Symbol>& candidates) const
{
    if (call.isMemberCall)
    {
        if (!call.objectBytes.Empty())
        {
            const Scope* callScope = m_request.scopeRoot
                                         ? FindEnclosingScope(m_request.scopeRoot, call.objectRange.startLine,
                                                              call.objectRange.startCharacter)
                                         : nullptr;
            const std::string objType = ResolveExpressionType(
                call.objectBytes, ExpressionTypeContext(callScope ? callScope : m_request.scopeRoot, m_ctx));
            const std::string_view cleanObj = CleanBaseTypeView(objType);
            const auto hierarchy = GetInheritedTypeHierarchy(cleanObj, m_ctx.request.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                if (const auto found = m_ctx.request.symbolTable.FindMemberSymbolPtr(typeName, call.memberName))
                {
                    for (const auto& sym : *found)
                    {
                        if (sym.type == SymbolType::Function)
                        {
                            candidates.push_back(sym);
                        }
                    }
                }
            }
        }
    }
    else
    {
        std::vector<ContainerInfo> containers;
        if (m_request.nodeIndex)
        {
            containers = m_request.nodeIndex->CollectLookupContainers(call.bytes.startByte, call.bytes.endByte, 0,
                                                                      m_request.sourceCode);
        }
        const auto& usings = GetUsingNamespaces(m_ctx.request);
        const auto inScope = FindSymbolsInScope(m_ctx.request.symbolTable, containers, call.calleeText, usings);
        for (const auto& sym : inScope)
        {
            if (sym.type == SymbolType::Function)
            {
                candidates.push_back(sym);
            }
        }
    }
}

bool DefiniteAssignmentVisitor::HasAnyUnassignedTrackedLocal(std::span<const DefiniteAssignmentArgumentFact> arguments,
                                                             const DefiniteAssignmentState& state) const
{
    return std::any_of(arguments.begin(), arguments.end(),
                       [&](const DefiniteAssignmentArgumentFact& arg)
                       {
                           return arg.isSimpleIdentifier && m_trackedLocals.contains(arg.name) &&
                                  !state.assignedVars.contains(arg.name);
                       });
}

bool DefiniteAssignmentVisitor::IsOutParameter(size_t i, const CallCandidateInfo& info) const
{
    const auto declaresOutAt = [i](const FunctionSignature& candidate)
    {
        if (i >= candidate.parameters.size())
        {
            return false;
        }
        const auto& param = candidate.parameters[i];
        return param.modifier == ParameterModifier::Out || param.isOutRef;
    };

    if (info.sig && declaresOutAt(*info.sig))
    {
        return true;
    }

    for (const auto& candidate : info.candidates)
    {
        if (std::holds_alternative<FunctionSignature>(candidate.signature) && declaresOutAt(candidate.GetFunction()))
        {
            return true;
        }
    }
    return false;
}

void DefiniteAssignmentVisitor::ProcessCall(const DefiniteAssignmentCallFact& call, DefiniteAssignmentState& state)
{
    const auto arguments = m_facts.ArgumentsOf(call);
    if (!HasAnyUnassignedTrackedLocal(arguments, state))
    {
        return;
    }

    std::vector<Symbol> candidates;
    CollectCallCandidates(call, candidates);

    std::vector<std::string> argTypes;
    argTypes.reserve(arguments.size());
    for (const auto& arg : arguments)
    {
        argTypes.push_back(ResolveExpressionType(arg.bytes, ExpressionTypeContext(m_request.scopeRoot, m_ctx)));
    }

    const auto best = ResolveBestOverload(candidates, argTypes, m_ctx.request.symbolTable);
    const FunctionSignature* sig =
        (best.bestCandidate && std::holds_alternative<FunctionSignature>(best.bestCandidate->signature))
            ? &best.bestCandidate->GetFunction()
            : (!candidates.empty() && std::holds_alternative<FunctionSignature>(candidates[0].signature)
                   ? &candidates[0].GetFunction()
                   : nullptr);

    const CallCandidateInfo info{candidates, sig};
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const auto& arg = arguments[i];
        if (!sig || IsOutParameter(i, info))
        {
            if (arg.isSimpleIdentifier && m_trackedLocals.contains(arg.name))
            {
                state.assignedVars.insert(arg.name);
            }
        }
        else
        {
            if (arg.isSimpleIdentifier && m_trackedLocals.contains(arg.name) && !state.assignedVars.contains(arg.name))
            {
                const uint64_t locationKey = (static_cast<uint64_t>(arg.range.startLine) << 32) |
                                             static_cast<uint64_t>(arg.range.startCharacter);
                if (m_reportedReads.insert(locationKey).second)
                {
                    m_ctx.EmitAtRange(arg.range, "as-warn-uninitialized-variable-read", arg.name,
                                      DiagnosticSeverity::Warning);
                }
            }
        }
    }
}

} // namespace angel_lsp::analysis
