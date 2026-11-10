#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <variant>
#include <vector>

namespace angel_lsp::analysis
{

void DefiniteAssignmentVisitor::CollectCallCandidates(const IndexedSyntaxNode* callee, const IndexedSyntaxNode* call,
                                                      std::vector<Symbol>& candidates) const
{
    const auto& gs = parser::GetGrammarSymbols();
    if (!callee)
    {
        return;
    }
    if (callee->symbol == gs.symMemberExpression)
    {
        const TSNode objNode = Syntax(FindField(callee, gs.fieldObject));
        const TSNode memNode = Syntax(FindField(callee, gs.fieldMember));
        if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
        {
            const TSPoint objStart = ts_node_start_point(objNode);
            const Scope* callScope =
                m_request.scopeRoot ? FindEnclosingScope(m_request.scopeRoot, objStart.row, objStart.column) : nullptr;
            std::string objType = ResolveExpressionType(
                objNode, ExpressionTypeContext(callScope ? callScope : m_request.scopeRoot, m_ctx));
            std::string cleanObj = CleanBaseType(objType);
            std::string_view memName = parser::NodeText(memNode, m_request.sourceCode);
            auto hierarchy = GetInheritedTypeHierarchy(cleanObj, m_ctx.request.symbolTable);
            for (const auto& typeName : hierarchy)
            {
                auto found = m_ctx.request.symbolTable.FindMemberSymbolPtr(typeName, memName);
                if (found)
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
        std::string funcName(parser::NodeText(Syntax(callee), m_request.sourceCode));
        auto inScope =
            FindSymbolsInScope(funcName, Syntax(call), m_request.sourceCode, m_ctx.request.symbolTable, &m_index);
        for (const auto& sym : inScope)
        {
            if (sym.type == SymbolType::Function)
            {
                candidates.push_back(sym);
            }
        }
    }
}

std::vector<const IndexedSyntaxNode*>
DefiniteAssignmentVisitor::CollectReadArguments(const IndexedSyntaxNode* call) const
{
    const auto& gs = parser::GetGrammarSymbols();
    const IndexedSyntaxNode* list = FindField(call, gs.fieldArguments);
    std::vector<const IndexedSyntaxNode*> arguments;
    for (auto* child = NthNamed(First(list), 0); child; child = NthNamed(Next(child), 0))
    {
        if (child->symbol != gs.symComment)
        {
            arguments.push_back(child);
        }
    }
    return arguments;
}

bool DefiniteAssignmentVisitor::HasAnyUnassignedTrackedLocal(const std::vector<const IndexedSyntaxNode*>& arguments,
                                                             const DefiniteAssignmentState& state) const
{
    return std::any_of(arguments.begin(), arguments.end(),
                       [&](const IndexedSyntaxNode* argument)
                       {
                           const std::string_view name = SimpleIdentifierName(argument);
                           return !name.empty() && m_trackedLocals.contains(name) && !state.assignedVars.contains(name);
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

void DefiniteAssignmentVisitor::QueueResolvedArgumentReads(const IndexedSyntaxNode* call,
                                                           const std::vector<const IndexedSyntaxNode*>& arguments)
{
    const auto& gs = parser::GetGrammarSymbols();
    std::vector<Symbol> candidates;
    CollectCallCandidates(FindField(call, gs.fieldFunction), call, candidates);

    std::vector<std::string> argTypes;
    argTypes.reserve(arguments.size());
    for (const IndexedSyntaxNode* arg : arguments)
    {
        argTypes.push_back(ResolveExpressionType(Syntax(arg), ExpressionTypeContext(m_request.scopeRoot, m_ctx)));
    }

    const auto best = ResolveBestOverload(candidates, argTypes, m_ctx.request.symbolTable);
    const FunctionSignature* sig =
        (best.bestCandidate && std::holds_alternative<FunctionSignature>(best.bestCandidate->signature))
            ? &best.bestCandidate->GetFunction()
            : (!candidates.empty() && std::holds_alternative<FunctionSignature>(candidates[0].signature)
                   ? &candidates[0].GetFunction()
                   : nullptr);

    const CallCandidateInfo info{candidates, sig};
    for (size_t i = arguments.size(); i > 0; --i)
    {
        const auto kind =
            !sig || IsOutParameter(i - 1, info) ? AssignmentReadKind::WriteOrRead : AssignmentReadKind::Read;
        QueueRead(arguments[i - 1], kind);
    }
}

void DefiniteAssignmentVisitor::QueueCallArgumentReads(const IndexedSyntaxNode* call,
                                                       const DefiniteAssignmentState& state)
{
    const auto arguments = CollectReadArguments(call);
    if (HasAnyUnassignedTrackedLocal(arguments, state))
    {
        QueueResolvedArgumentReads(call, arguments);
        return;
    }
    for (auto it = arguments.rbegin(); it != arguments.rend(); ++it)
    {
        QueueRead(*it);
    }
}

} // namespace angel_lsp::analysis
