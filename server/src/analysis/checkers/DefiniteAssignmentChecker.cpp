#include "analysis/checkers/DefiniteAssignmentChecker.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeExtraction.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Combines initialized names from viable branches according to AngelScript's union semantics.
 */
static ankerl::unordered_dense::set<std::string_view>
MergeBranchStates(const ankerl::unordered_dense::set<std::string_view>& a,
                  const ankerl::unordered_dense::set<std::string_view>& b)
{
    ankerl::unordered_dense::set<std::string_view> result = a;
    for (const auto& item : b)
    {
        result.insert(item);
    }
    return result;
}

/**
 * @brief Evaluates extracted statement edges in execution order while tracking initialized locals.
 */
class DefiniteAssignmentVisitor
{
  public:
    DefiniteAssignmentVisitor(const AstCheckContext& request, DiagnosticContext& ctx)
        : m_request(request), m_ctx(ctx), m_index(*request.nodeIndex)
    {
    }

    void AnalyzeFunction(SyntaxNodeId callable)
    {
        m_trackedLocals.clear();
        m_reportedReads.clear();

        const auto& gs = parser::GetGrammarSymbols();
        const Operand* entry = m_index.SyntaxNode(callable);
        const Operand* body = FindField(entry, gs.fieldBody);
        if (!body)
        {
            body = FindOperand(entry, [&](const Operand& item) { return item.symbol == gs.symStatementBlock; });
        }

        DefiniteAssignmentState state;
        Schedule(body, state);
        while (!m_work.empty())
        {
            if (Step(m_work.back()))
            {
                m_work.pop_back();
            }
        }
    }

  private:
    using Operand = IndexedSyntaxNode;

    const AstCheckContext& m_request;
    DiagnosticContext& m_ctx;
    const NodeIndex& m_index;
    std::deque<AssignmentWorkFrame> m_work;
    std::vector<AssignmentReadTask> m_reads;
    ankerl::unordered_dense::set<std::string_view> m_trackedLocals;
    ankerl::unordered_dense::set<uint64_t> m_reportedReads;

    const Operand* First(const Operand* node) const
    {
        return node ? m_index.SyntaxNode(node->first) : nullptr;
    }

    const Operand* Next(const Operand* operand) const
    {
        return operand ? m_index.SyntaxNode(operand->next) : nullptr;
    }

    template <typename Predicate> const Operand* FindOperand(const Operand* node, Predicate predicate) const
    {
        for (auto* operand = First(node); operand; operand = Next(operand))
        {
            if (predicate(*operand))
            {
                return operand;
            }
        }
        return nullptr;
    }

    const Operand* FindField(const Operand* node, TSFieldId field) const
    {
        return field == 0 ? nullptr
                          : FindOperand(node, [field](const Operand& operand) { return operand.field == field; });
    }

    const Operand* NthNamed(const Operand* operand, unsigned ordinal) const
    {
        for (; operand; operand = Next(operand))
        {
            if (operand->named && ordinal-- == 0)
            {
                return operand;
            }
        }
        return nullptr;
    }

    const Operand* FindFieldOrNamed(const Operand* node, TSFieldId field, unsigned ordinal = 0) const
    {
        const Operand* operand = FindField(node, field);
        return operand ? operand : NthNamed(First(node), ordinal);
    }

    TSNode Syntax(const Operand* operand) const
    {
        return operand ? m_index.SyntaxHandle(*operand) : TSNode{};
    }

    void Schedule(const Operand* operand, DefiniteAssignmentState& state)
    {
        if (!operand || state.isTerminated || state.hasReturned)
        {
            return;
        }
        if (operand->statement)
        {
            m_work.push_back({operand, state, {}, {}, First(operand)});
        }
        else
        {
            CheckExpressionReads(operand, state);
        }
    }

    std::string_view SimpleIdentifierName(const Operand* operand) const
    {
        if (!operand)
        {
            return {};
        }
        const auto& gs = parser::GetGrammarSymbols();
        if (operand->symbol == gs.symScopedIdentifier)
        {
            operand = NthNamed(First(operand), 0);
            if (NthNamed(Next(operand), 0))
            {
                return {};
            }
        }
        return operand && operand->symbol == gs.symIdentifier
                   ? parser::GetNodeTextView(Syntax(operand), m_request.sourceCode)
                   : std::string_view{};
    }

    void CheckIdentifierRead(const Operand* operand, const DefiniteAssignmentState& state)
    {
        std::string_view name = SimpleIdentifierName(operand);
        if (!name.empty() && m_trackedLocals.contains(name) && !state.assignedVars.contains(name))
        {
            const TSNode node = Syntax(operand);
            if (ts_node_is_null(node))
            {
                return;
            }
            TSPoint start = ts_node_start_point(node);
            TSPoint end = ts_node_end_point(node);
            const uint64_t locationKey = (static_cast<uint64_t>(start.row) << 32) | static_cast<uint64_t>(start.column);
            if (m_reportedReads.insert(locationKey).second)
            {
                m_ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-warn-uninitialized-variable-read",
                                  name, DiagnosticSeverity::Warning);
            }
        }
    }

    bool MarkTrackedAssignment(const Operand* operand, DefiniteAssignmentState& state)
    {
        std::string_view name = SimpleIdentifierName(operand);
        if (name.empty() || !m_trackedLocals.contains(name))
        {
            return false;
        }
        state.assignedVars.insert(name);
        return true;
    }

    void QueueRead(const Operand* operand, AssignmentReadKind kind = AssignmentReadKind::Read)
    {
        if (operand)
        {
            m_reads.push_back({operand, kind});
        }
    }

    void QueueAssignmentReads(const Operand* node)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const Operand* op = FindField(node, gs.fieldOperator);
        QueueRead(FindField(node, gs.fieldLeft),
                  op && op->symbol == gs.tokAssign ? AssignmentReadKind::WriteOrRead : AssignmentReadKind::Read);
        QueueRead(FindField(node, gs.fieldRight));
    }

    void CollectCallCandidates(const Operand* callee, const Operand* call, std::vector<Symbol>& candidates) const
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
                const Scope* callScope = m_request.scopeRoot
                                             ? FindEnclosingScope(m_request.scopeRoot, objStart.row, objStart.column)
                                             : nullptr;
                std::string objType = ResolveExpressionType(
                    objNode, ExpressionTypeContext(callScope ? callScope : m_request.scopeRoot, m_ctx));
                std::string cleanObj = CleanBaseType(objType);
                std::string_view memName = parser::GetNodeTextView(memNode, m_request.sourceCode);
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
            std::string funcName(parser::GetNodeTextView(Syntax(callee), m_request.sourceCode));
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

    std::vector<const Operand*> CollectReadArguments(const Operand* call) const
    {
        const auto& gs = parser::GetGrammarSymbols();
        const Operand* list = FindField(call, gs.fieldArguments);
        std::vector<const Operand*> arguments;
        for (auto* child = NthNamed(First(list), 0); child; child = NthNamed(Next(child), 0))
        {
            if (child->symbol != gs.symComment)
            {
                arguments.push_back(child);
            }
        }
        return arguments;
    }

    bool HasAnyUnassignedTrackedLocal(const std::vector<const Operand*>& arguments,
                                      const DefiniteAssignmentState& state) const
    {
        return std::any_of(arguments.begin(), arguments.end(),
                           [&](const Operand* argument)
                           {
                               const std::string_view name = SimpleIdentifierName(argument);
                               return !name.empty() && m_trackedLocals.contains(name) &&
                                      !state.assignedVars.contains(name);
                           });
    }

    /**
     * @brief Is out parameter.
     * @param[in] i Iteration index or parameter position.
     * @param[in] info Candidate symbol or call information.
     * @return True if the condition is satisfied; false otherwise.
     */
    bool IsOutParameter(size_t i, const CallCandidateInfo& info) const
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
            if (std::holds_alternative<FunctionSignature>(candidate.signature) &&
                declaresOutAt(candidate.GetFunction()))
            {
                return true;
            }
        }
        return false;
    }

    void QueueResolvedArgumentReads(const Operand* call, const std::vector<const Operand*>& arguments)
    {
        const auto& gs = parser::GetGrammarSymbols();
        std::vector<Symbol> candidates;
        CollectCallCandidates(FindField(call, gs.fieldFunction), call, candidates);

        std::vector<std::string> argTypes;
        argTypes.reserve(arguments.size());
        for (const Operand* arg : arguments)
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

    void QueueCallArgumentReads(const Operand* call, const DefiniteAssignmentState& state)
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

    void QueueExpressionReads(const Operand* node, const DefiniteAssignmentState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol symbol = node->symbol;
        if (symbol == gs.symIdentifier || symbol == gs.symScopedIdentifier)
        {
            CheckIdentifierRead(node, state);
        }
        else if (symbol == gs.symAssignmentExpression)
        {
            QueueAssignmentReads(node);
        }
        else if (symbol == gs.symCallExpression)
        {
            QueueRead(node, AssignmentReadKind::CallArguments);
            QueueRead(FindField(node, gs.fieldFunction));
        }
        else if (symbol == gs.symMemberExpression)
        {
            QueueRead(FindField(node, gs.fieldObject));
        }
        else
        {
            QueueRead(NthNamed(First(node), 0), AssignmentReadKind::ReadSiblings);
        }
    }

    void EvaluateReadTask(AssignmentReadTask task, DefiniteAssignmentState& state)
    {
        switch (task.kind)
        {
        case AssignmentReadKind::CallArguments:
            QueueCallArgumentReads(task.operand, state);
            return;
        case AssignmentReadKind::WriteOrRead:
            if (MarkTrackedAssignment(task.operand, state))
            {
                return;
            }
            break;
        case AssignmentReadKind::ReadSiblings:
            QueueRead(NthNamed(Next(task.operand), 0), AssignmentReadKind::ReadSiblings);
            break;
        case AssignmentReadKind::Read:
            break;
        }
        QueueExpressionReads(task.operand, state);
    }

    void CheckExpressionReads(const Operand* node, DefiniteAssignmentState& state)
    {
        if (m_trackedLocals.empty())
        {
            return;
        }
        QueueRead(node);
        while (!m_reads.empty())
        {
            const auto task = m_reads.back();
            m_reads.pop_back();
            EvaluateReadTask(task, state);
        }
    }

    void AnalyzeVariableDeclarator(const Operand* node, bool isPrimitive, DefiniteAssignmentState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const Operand* name = FindFieldOrNamed(node, gs.fieldName);
        if (!name)
        {
            return;
        }

        std::string_view varName = parser::GetNodeTextView(Syntax(name), m_request.sourceCode);
        const Operand* init = FindField(node, gs.fieldValue);
        if (!init)
        {
            init = FindOperand(node, [](const Operand& item) { return item.afterAssign; });
        }
        if (init)
        {
            CheckExpressionReads(init, state);
            state.assignedVars.insert(varName);
        }
        else if (isPrimitive)
        {
            m_trackedLocals.insert(varName);
        }
        else
        {
            state.assignedVars.insert(varName);
        }
    }

    void AnalyzeVariableDeclaration(const Operand* node, DefiniteAssignmentState& state)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const Operand* type = FindField(node, gs.fieldVarType);
        if (!type)
        {
            type = FindField(node, gs.fieldType);
        }

        bool isPrimitive = false;
        if (type)
        {
            const TypeExtractionResult info = ExtractTypeInfoFromAST(Syntax(type), m_request.sourceCode, &m_index);
            isPrimitive =
                IsPrimitiveTypeName(info.baseTypeName) && !info.isArray && !info.isHandle && info.templateName.empty();
        }
        for (auto* operand = First(node); operand; operand = Next(operand))
        {
            if (operand->symbol == gs.symVariableDeclarator)
            {
                AnalyzeVariableDeclarator(operand, isPrimitive, state);
            }
        }
    }

    static void MergeIfBranches(AssignmentWorkFrame& frame)
    {
        auto& thenState = frame.branch;
        auto& elseState = frame.alternate;
        if (thenState.hasReturned && elseState.hasReturned)
        {
            frame.state.hasReturned = true;
        }
        else if (thenState.hasReturned)
        {
            frame.state = std::move(elseState);
        }
        else if (elseState.hasReturned)
        {
            frame.state = std::move(thenState);
        }
        else
        {
            frame.state.assignedVars = MergeBranchStates(thenState.assignedVars, elseState.assignedVars);
        }
    }

    bool StepIf(AssignmentWorkFrame& frame)
    {
        const auto& gs = parser::GetGrammarSymbols();
        switch (frame.phase++)
        {
        case 0:
            CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition), frame.state);
            frame.branch = frame.state;
            Schedule(FindField(frame.node, gs.fieldConsequence), frame.branch);
            return false;
        case 1:
            frame.alternate = frame.state;
            Schedule(FindField(frame.node, gs.fieldAlternative), frame.alternate);
            return false;
        default:
            MergeIfBranches(frame);
            return true;
        }
    }

    bool StepBlock(AssignmentWorkFrame& frame, bool clause)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (frame.phase == 0 && clause && !frame.node->defaultClause)
        {
            frame.next = Next(NthNamed(frame.next, 0));
        }
        frame.phase = 1;
        const Operand* operand = NthNamed(frame.next, 0);
        if (!operand || frame.state.hasReturned || frame.state.isTerminated ||
            (clause && operand->symbol == gs.symBreakStatement))
        {
            return true;
        }
        frame.next = Next(operand);
        Schedule(operand, frame.state);
        return false;
    }

    void MergeLoopBody(AssignmentWorkFrame& frame, TSNode condition, bool isFor)
    {
        frame.state.assignedVars = MergeBranchStates(frame.state.assignedVars, frame.branch.assignedVars);
        const std::string_view raw = parser::GetNodeTextView(condition, m_request.sourceCode);
        const std::string_view value = isFor ? parser::Trim(raw) : raw;
        if ((value == "true" || (isFor && (value.empty() || value == ";"))) && !frame.branch.hasReturned &&
            !frame.branch.isTerminated)
        {
            frame.state = std::move(frame.branch);
        }
    }

    bool StepLoop(AssignmentWorkFrame& frame, TSSymbol symbol)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const bool isFor = symbol == gs.symForStatement;
        switch (frame.phase++)
        {
        case 0:
            if (isFor)
            {
                Schedule(FindField(frame.node, gs.fieldInit), frame.state);
            }
            return false;
        case 1:
            frame.next =
                isFor ? FindField(frame.node, gs.fieldCondition) : FindFieldOrNamed(frame.node, gs.fieldCondition);
            CheckExpressionReads(frame.next, frame.state);
            frame.branch = frame.state;
            Schedule(FindField(frame.node, gs.fieldBody), frame.branch);
            return false;
        default:
            if (isFor)
            {
                CheckExpressionReads(FindField(frame.node, gs.fieldUpdate), frame.branch);
            }
            MergeLoopBody(frame, Syntax(frame.next), isFor);
            return true;
        }
    }

    bool StepDoWhile(AssignmentWorkFrame& frame)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (frame.phase++ == 0)
        {
            Schedule(FindField(frame.node, gs.fieldBody), frame.state);
            return false;
        }
        CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition, 1), frame.state);
        return true;
    }

    bool StepSwitch(AssignmentWorkFrame& frame)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (frame.phase == 0)
        {
            CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldCondition), frame.state);
            frame.alternate.hasReturned = true;
        }
        else if (!frame.branch.hasReturned)
        {
            frame.alternate.hasReturned = false;
            for (const auto& name : frame.branch.assignedVars)
            {
                frame.alternate.assignedVars.insert(name);
            }
        }

        while (frame.next && frame.next->symbol != gs.symCaseClause)
        {
            frame.next = Next(frame.next);
        }
        if (const Operand* clause = frame.next)
        {
            frame.phase = 1;
            frame.next = Next(clause);
            frame.branch = frame.state;
            Schedule(clause, frame.branch);
            return false;
        }
        if (frame.phase != 0)
        {
            if (frame.alternate.hasReturned)
            {
                frame.state.hasReturned = true;
            }
            else
            {
                frame.state.assignedVars = std::move(frame.alternate.assignedVars);
            }
        }
        return true;
    }

    bool Step(AssignmentWorkFrame& frame)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol symbol = frame.node->symbol;
        if (symbol == gs.symStatementBlock || symbol == gs.symCaseClause)
        {
            return StepBlock(frame, symbol == gs.symCaseClause);
        }
        if (symbol == gs.symIfStatement)
        {
            return StepIf(frame);
        }
        if (symbol == gs.symWhileStatement || symbol == gs.symForStatement)
        {
            return StepLoop(frame, symbol);
        }
        if (symbol == gs.symDoWhileStatement)
        {
            return StepDoWhile(frame);
        }
        if (symbol == gs.symSwitchStatement)
        {
            return StepSwitch(frame);
        }
        StepLeaf(frame, symbol);
        return true;
    }

    void StepLeaf(AssignmentWorkFrame& frame, TSSymbol symbol)
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (symbol == gs.symExpressionStatement)
        {
            CheckExpressionReads(NthNamed(frame.next, 0), frame.state);
        }
        else if (symbol == gs.symVariableDeclaration)
        {
            AnalyzeVariableDeclaration(frame.node, frame.state);
        }
        else if (symbol == gs.symReturnStatement)
        {
            CheckExpressionReads(FindFieldOrNamed(frame.node, gs.fieldValue), frame.state);
            frame.state.hasReturned = true;
            frame.state.isTerminated = true;
        }
        else if (symbol == gs.symBreakStatement || symbol == gs.symContinueStatement)
        {
            frame.state.isTerminated = true;
        }
    }
};

void CheckDefiniteAssignment(const AstCheckContext& request, DiagnosticContext& ctx)
{
    if (!request.nodeIndex || request.sourceCode.empty() ||
        utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    DefiniteAssignmentVisitor visitor(request, ctx);
    for (const bool lambdas : {false, true})
    {
        for (const auto callable : request.nodeIndex->AssignmentCallables(lambdas))
        {
            visitor.AnalyzeFunction(callable);
        }
    }
}
} // namespace angel_lsp::analysis
