#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/OverloadResolver.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include "utils/Utils.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief Node text as an owning string.
 *
 * Kept per translation unit rather than shared with ASTUtils::NodeText, which returns a
 * string_view. The two are not interchangeable: callers here store the result, concatenate
 * it, and use it after the node has gone out of scope, so handing them a view would trade a
 * duplicated three-line function for a lifetime question at several dozen call sites.
 * Deduplicating it was attempted and reverted for exactly that reason.
 */
std::string NodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }

    const uint32_t start = ts_node_start_byte(node);
    const uint32_t end = ts_node_end_byte(node);
    if (start >= end || end > sourceCode.size())
    {
        return "";
    }
    return std::string(sourceCode.substr(start, end - start));
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string GetSimpleIdentifierName(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return "";
    }
    std::string_view type = ts_node_type(node);
    if (type == "identifier")
    {
        return NodeText(node, sourceCode);
    }
    if (type == "scoped_identifier")
    {
        uint32_t count = ts_node_named_child_count(node);
        if (count == 1)
        {
            TSNode child = ts_node_named_child(node, 0);
            if (std::string_view(ts_node_type(child)) == "identifier")
            {
                return NodeText(child, sourceCode);
            }
        }
    }
    return "";
}

struct FlowState
{
    ankerl::unordered_dense::set<std::string> assignedVars;
    bool hasReturned = false;
    bool isTerminated = false;
};

/**
 * @brief Merges two branch states by UNION, which is AngelScript's rule and not C#'s.
 *
 * This intersected, implementing "definitely assigned on every path" - the rule C# and
 * Java enforce as an error. AngelScript's is far weaker, and it is a warning: it fires only
 * when NO assignment precedes the read at all. Measured, and the two middle lines are the
 * ones that settle it:
 *
 *     uint c; Use(c);                        WARNING: 'c' is not initialized.
 *     uint c; Use(c); c = 5;                 WARNING - the read comes first
 *     uint c; if (true)  { c = 5; } Use(c);  clean
 *     uint c; if (false) { c = 5; } Use(c);  clean - conditional, and even unreachable
 *     uint c; for (...) { if (...) c = 5; } Use(c);   clean
 *     uint c; while (true) { c = 5; break; } Use(c);  clean
 *
 * So an assignment in one arm is enough, and the join has to keep it rather than require
 * both arms to agree. Intersecting produced 400 findings over the corpus against the
 * compiler's own 7, on the ordinary shape of assigning inside a loop and reading after it.
 */
ankerl::unordered_dense::set<std::string> MergeBranchStates(const ankerl::unordered_dense::set<std::string>& a,
                                                            const ankerl::unordered_dense::set<std::string>& b)
{
    ankerl::unordered_dense::set<std::string> result = a;
    for (const auto& item : b)
    {
        result.insert(item);
    }
    return result;
}

struct CallCandidateInfo
{
    const std::vector<Symbol>& candidates;
    const FunctionSignature* sig = nullptr;
};

class DefiniteAssignmentVisitor
{
  public:
    DefiniteAssignmentVisitor(const DefiniteAssignmentCheckRequest& request, DiagnosticContext& ctx)
        : m_request(request), m_ctx(ctx)
    {
    }

    void AnalyzeFunction(TSNode funcNode)
    {
        m_trackedLocals.clear();
        m_reportedReads.clear();

        TSNode bodyNode = parser::GetChildByField(funcNode, parser::fields::Body);
        if (ts_node_is_null(bodyNode))
        {
            uint32_t count = ts_node_named_child_count(funcNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_named_child(funcNode, i);
                if (std::string_view(ts_node_type(child)) == "statement_block")
                {
                    bodyNode = child;
                    break;
                }
            }
        }

        if (ts_node_is_null(bodyNode))
        {
            return;
        }

        FlowState state;
        AnalyzeStatement(bodyNode, state);
    }

  private:
    const DefiniteAssignmentCheckRequest& m_request;
    DiagnosticContext& m_ctx;
    ankerl::unordered_dense::set<std::string> m_trackedLocals;
    ankerl::unordered_dense::set<std::string> m_reportedReads;

    void CheckIdentifierRead(TSNode node, const FlowState& state)
    {
        std::string name = GetSimpleIdentifierName(node, m_request.sourceCode);
        if (!name.empty() && m_trackedLocals.contains(name) && !state.assignedVars.contains(name))
        {
            TSPoint start = ts_node_start_point(node);
            TSPoint end = ts_node_end_point(node);
            std::string locationKey = name + ":" + std::to_string(start.row) + ":" + std::to_string(start.column);
            if (m_reportedReads.insert(locationKey).second)
            {
                m_ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-warn-uninitialized-variable-read",
                                  name, DiagnosticSeverity::Warning);
            }
        }
    }

    void CheckAssignmentExpressionReads(TSNode node, FlowState& state, int depth)
    {
        TSNode left = parser::GetChildByField(node, parser::fields::Left);
        TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
        TSNode right = parser::GetChildByField(node, parser::fields::Right);
        std::string op = NodeText(opNode, m_request.sourceCode);

        // Evaluate right-hand side reads first
        CheckExpressionReads(right, state, depth + 1);

        if (op == "=")
        {
            std::string varName = GetSimpleIdentifierName(left, m_request.sourceCode);
            if (!varName.empty() && m_trackedLocals.contains(varName))
            {
                state.assignedVars.insert(varName);
                return;
            }
            // Non-bare identifier on left (e.g. obj.x = val, arr[i] = val)
            CheckExpressionReads(left, state, depth + 1);
        }
        else
        {
            // Compound assignment (+=, -=, etc.): left is read before being written
            CheckExpressionReads(left, state, depth + 1);
        }
    }

    void CollectCallCandidates(TSNode funcNode, TSNode callNode, std::vector<Symbol>& candidates) const
    {
        std::string_view funcType = ts_node_type(funcNode);
        if (funcType == "member_expression")
        {
            TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
            TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
            if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
            {
                const TSPoint objStart = ts_node_start_point(objNode);
                const Scope* callScope = m_request.scopeRoot
                                             ? FindEnclosingScope(m_request.scopeRoot, objStart.row, objStart.column)
                                             : nullptr;
                std::string objType = ResolveExpressionType(objNode, {callScope ? callScope : m_request.scopeRoot,
                                                                      m_ctx.request.symbolTable, m_request.sourceCode,
                                                                      m_ctx.request.fileUri});
                std::string cleanObj = CleanBaseType(objType);
                std::string memName = NodeText(memNode, m_request.sourceCode);
                auto hierarchy = GetInheritedTypeHierarchy(cleanObj, m_ctx.request.symbolTable);
                for (const auto& typeName : hierarchy)
                {
                    auto found = m_ctx.request.symbolTable.FindSymbolsPtr(typeName + "::" + memName);
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
            std::string funcName = NodeText(funcNode, m_request.sourceCode);
            auto inScope = FindSymbolsInScope(funcName, callNode, m_request.sourceCode, m_ctx.request.symbolTable);
            for (const auto& sym : inScope)
            {
                if (sym.type == SymbolType::Function)
                {
                    candidates.push_back(sym);
                }
            }
        }
    }

    void ExtractRawArgNodes(TSNode argsNode, std::vector<TSNode>& argNodes) const
    {
        uint32_t rawChildCount = ts_node_child_count(argsNode);
        for (uint32_t i = 0; i < rawChildCount; ++i)
        {
            TSNode child = ts_node_child(argsNode, i);
            std::string_view ct = ts_node_type(child);
            if (ct != "(" && ct != ")" && ct != "," && ct != "comment")
            {
                argNodes.push_back(child);
            }
        }
    }

    bool HasAnyUnassignedTrackedLocal(const std::vector<TSNode>& argNodes, const FlowState& state) const
    {
        if (m_trackedLocals.empty())
        {
            return false;
        }
        for (TSNode arg : argNodes)
        {
            std::string varName = GetSimpleIdentifierName(arg, m_request.sourceCode);
            if (!varName.empty() && m_trackedLocals.contains(varName) && !state.assignedVars.contains(varName))
            {
                return true;
            }
        }
        return false;
    }

    bool IsOutParameter(size_t i, const CallCandidateInfo& info) const
    {
        const auto declaresOutAt = [i](const FunctionSignature& candidate)
        {
            if (i >= candidate.parameters.size())
            {
                return false;
            }
            const auto& param = candidate.parameters[i];
            return param.modifier == ParameterModifier::Out || param.typeName.find("&out") != std::string::npos ||
                   param.rawText.find("&out") != std::string::npos;
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

    void CheckCallArguments(const std::vector<TSNode>& argNodes, const CallCandidateInfo& info, FlowState& state,
                            int depth)
    {
        for (size_t i = 0; i < argNodes.size(); ++i)
        {
            if (IsOutParameter(i, info))
            {
                std::string varName = GetSimpleIdentifierName(argNodes[i], m_request.sourceCode);
                if (!varName.empty() && m_trackedLocals.contains(varName))
                {
                    state.assignedVars.insert(varName);
                    continue;
                }
            }

            if (!info.sig)
            {
                const std::string bareName = GetSimpleIdentifierName(argNodes[i], m_request.sourceCode);
                if (!bareName.empty() && m_trackedLocals.contains(bareName))
                {
                    state.assignedVars.insert(bareName);
                    continue;
                }
            }

            CheckExpressionReads(argNodes[i], state, depth + 1);
        }
    }

    void CheckCallExpressionReads(TSNode node, FlowState& state, int depth)
    {
        TSNode funcNode = parser::GetChildByField(node, parser::fields::Function);
        TSNode argsNode = parser::GetChildByField(node, parser::fields::Arguments);

        CheckExpressionReads(funcNode, state, depth + 1);

        if (ts_node_is_null(argsNode))
        {
            return;
        }

        std::vector<TSNode> argNodes;
        ExtractRawArgNodes(argsNode, argNodes);

        if (!HasAnyUnassignedTrackedLocal(argNodes, state))
        {
            for (TSNode arg : argNodes)
            {
                CheckExpressionReads(arg, state, depth + 1);
            }
            return;
        }

        std::vector<Symbol> candidates;
        CollectCallCandidates(funcNode, node, candidates);

        std::vector<std::string> argTypes;
        argTypes.reserve(argNodes.size());
        for (TSNode arg : argNodes)
        {
            argTypes.push_back(ResolveExpressionType(
                arg, {m_request.scopeRoot, m_ctx.request.symbolTable, m_request.sourceCode, m_ctx.request.fileUri}));
        }

        auto best = ResolveBestOverload(candidates, argTypes, m_ctx.request.symbolTable);
        const FunctionSignature* sig =
            (best.bestCandidate && std::holds_alternative<FunctionSignature>(best.bestCandidate->signature))
                ? &best.bestCandidate->GetFunction()
                : (!candidates.empty() && std::holds_alternative<FunctionSignature>(candidates[0].signature)
                       ? &candidates[0].GetFunction()
                       : nullptr);

        CallCandidateInfo info{candidates, sig};
        CheckCallArguments(argNodes, info, state, depth);
    }

    void CheckExpressionReads(TSNode node, FlowState& state, int depth = 0)
    {
        if (depth > k_maxAstDepth || ts_node_is_null(node))
        {
            return;
        }

        std::string_view type = ts_node_type(node);

        if (type == "identifier" || type == "scoped_identifier")
        {
            CheckIdentifierRead(node, state);
            return;
        }

        if (type == "assignment_expression")
        {
            CheckAssignmentExpressionReads(node, state, depth);
            return;
        }

        if (type == "call_expression")
        {
            CheckCallExpressionReads(node, state, depth);
            return;
        }

        if (type == "member_expression")
        {
            TSNode objNode = parser::GetChildByField(node, parser::fields::Object);
            CheckExpressionReads(objNode, state, depth + 1);
            return;
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            CheckExpressionReads(ts_node_named_child(node, i), state, depth + 1);
        }
    }

    void AnalyzeBlock(TSNode node, FlowState& state, int depth)
    {
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count && !state.isTerminated && !state.hasReturned; ++i)
        {
            AnalyzeStatement(ts_node_named_child(node, i), state, depth + 1);
        }
    }

    void AnalyzeVariableDeclarator(TSNode child, bool isPrimitive, FlowState& state, int depth)
    {
        TSNode nameNode = parser::GetChildByField(child, parser::fields::Name);
        if (ts_node_is_null(nameNode) && ts_node_named_child_count(child) > 0)
        {
            nameNode = ts_node_named_child(child, 0);
        }
        if (ts_node_is_null(nameNode))
        {
            return;
        }

        std::string varName = NodeText(nameNode, m_request.sourceCode);
        TSNode initNode = parser::GetChildByField(child, parser::fields::Value);
        if (ts_node_is_null(initNode))
        {
            uint32_t rawCount = ts_node_child_count(child);
            bool sawEq = false;
            for (uint32_t j = 0; j < rawCount; ++j)
            {
                TSNode rawChild = ts_node_child(child, j);
                if (sawEq)
                {
                    initNode = rawChild;
                    break;
                }
                if (std::string_view(ts_node_type(rawChild)) == "=")
                {
                    sawEq = true;
                }
            }
        }

        if (!ts_node_is_null(initNode))
        {
            CheckExpressionReads(initNode, state, depth + 1);
            state.assignedVars.insert(varName);
        }
        else
        {
            if (isPrimitive)
            {
                m_trackedLocals.insert(varName);
            }
            else
            {
                state.assignedVars.insert(varName);
            }
        }
    }

    void AnalyzeVariableDeclaration(TSNode node, FlowState& state, int depth)
    {
        TSNode typeNode = parser::GetChildByField(node, parser::fields::VarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(node, parser::fields::Type);
        }

        bool isPrimitive = false;
        if (!ts_node_is_null(typeNode))
        {
            TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, m_request.sourceCode);
            isPrimitive = (IsPrimitiveTypeName(typeInfo.baseTypeName) && !typeInfo.isArray && !typeInfo.isHandle &&
                           typeInfo.templateName.empty());
        }

        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (std::string_view(ts_node_type(child)) == "variable_declarator")
            {
                AnalyzeVariableDeclarator(child, isPrimitive, state, depth);
            }
        }
    }

    void AnalyzeIfStatement(TSNode node, FlowState& state, int depth)
    {
        TSNode cond = ts_node_named_child(node, 0);
        TSNode consequence = parser::GetChildByField(node, parser::fields::Consequence);
        TSNode alternative = parser::GetChildByField(node, parser::fields::Alternative);

        CheckExpressionReads(cond, state, depth + 1);

        FlowState thenState = state;
        AnalyzeStatement(consequence, thenState, depth + 1);

        FlowState elseState = state;
        if (!ts_node_is_null(alternative))
        {
            AnalyzeStatement(alternative, elseState, depth + 1);
        }

        if (thenState.hasReturned && elseState.hasReturned)
        {
            state.hasReturned = true;
        }
        else if (thenState.hasReturned)
        {
            state = elseState;
        }
        else if (elseState.hasReturned)
        {
            state = thenState;
        }
        else
        {
            state.assignedVars = MergeBranchStates(thenState.assignedVars, elseState.assignedVars);
        }
    }

    void AnalyzeWhileStatement(TSNode node, FlowState& state, int depth)
    {
        TSNode cond = ts_node_named_child(node, 0);
        TSNode body = parser::GetChildByField(node, parser::fields::Body);

        CheckExpressionReads(cond, state, depth + 1);

        FlowState bodyState = state;
        AnalyzeStatement(body, bodyState, depth + 1);

        state.assignedVars = MergeBranchStates(state.assignedVars, bodyState.assignedVars);

        std::string condText = NodeText(cond, m_request.sourceCode);
        if (condText == "true" && !bodyState.hasReturned && !bodyState.isTerminated)
        {
            state = bodyState;
        }
    }

    void AnalyzeDoWhileStatement(TSNode node, FlowState& state, int depth)
    {
        TSNode body = parser::GetChildByField(node, parser::fields::Body);
        TSNode cond = ts_node_named_child(node, 1);

        AnalyzeStatement(body, state, depth + 1);
        CheckExpressionReads(cond, state, depth + 1);
    }

    void AnalyzeForStatement(TSNode node, FlowState& state, int depth)
    {
        TSNode init = parser::GetChildByField(node, parser::fields::Init);
        TSNode cond = parser::GetChildByField(node, parser::fields::Condition);
        TSNode step = parser::GetChildByField(node, parser::fields::Update);
        TSNode body = parser::GetChildByField(node, parser::fields::Body);

        if (!ts_node_is_null(init))
        {
            if (std::string_view(ts_node_type(init)) == "assignment_expression")
            {
                CheckExpressionReads(init, state, depth + 1);
            }
            else
            {
                AnalyzeStatement(init, state, depth + 1);
            }
        }

        CheckExpressionReads(cond, state, depth + 1);

        FlowState bodyState = state;
        AnalyzeStatement(body, bodyState, depth + 1);
        CheckExpressionReads(step, bodyState, depth + 1);

        state.assignedVars = MergeBranchStates(state.assignedVars, bodyState.assignedVars);

        std::string rawCond = NodeText(cond, m_request.sourceCode);
        std::string_view condText = Trim(rawCond);
        if ((condText.empty() || condText == "true" || condText == ";") && !bodyState.hasReturned &&
            !bodyState.isTerminated)
        {
            state = bodyState;
        }
    }

    FlowState AnalyzeCaseClause(TSNode child, const FlowState& parentState, int depth)
    {
        TSNode kw = ts_node_child(child, 0);
        FlowState caseState = parentState;
        uint32_t stmtCount = ts_node_named_child_count(child);
        uint32_t first = (std::string_view(ts_node_type(kw)) == "default") ? 0u : 1u;
        for (uint32_t j = first; j < stmtCount && !caseState.hasReturned; ++j)
        {
            TSNode stmtChild = ts_node_named_child(child, j);
            if (std::string_view(ts_node_type(stmtChild)) == "break_statement")
            {
                break;
            }
            AnalyzeStatement(stmtChild, caseState, depth + 1);
        }
        return caseState;
    }

    void MergeSwitchCaseStates(const std::vector<FlowState>& caseStates, FlowState& state)
    {
        if (caseStates.empty())
        {
            return;
        }

        bool allReturn = true;
        std::vector<ankerl::unordered_dense::set<std::string>> liveCaseSets;

        for (const auto& cs : caseStates)
        {
            if (!cs.hasReturned)
            {
                allReturn = false;
                liveCaseSets.push_back(cs.assignedVars);
            }
        }

        if (allReturn)
        {
            state.hasReturned = true;
        }
        else if (!liveCaseSets.empty())
        {
            ankerl::unordered_dense::set<std::string> merged = liveCaseSets[0];
            for (size_t i = 1; i < liveCaseSets.size(); ++i)
            {
                merged = MergeBranchStates(merged, liveCaseSets[i]);
            }
            state.assignedVars = std::move(merged);
        }
    }

    void AnalyzeSwitchStatement(TSNode node, FlowState& state, int depth)
    {
        TSNode cond = ts_node_named_child(node, 0);
        CheckExpressionReads(cond, state, depth + 1);

        std::vector<FlowState> caseStates;
        uint32_t count = ts_node_named_child_count(node);

        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(node, i);
            if (std::string_view(ts_node_type(child)) == "case_clause")
            {
                caseStates.push_back(AnalyzeCaseClause(child, state, depth));
            }
        }

        MergeSwitchCaseStates(caseStates, state);
    }

    bool AnalyzeLoopStatement(std::string_view type, TSNode node, FlowState& state, int depth)
    {
        if (type == "while_statement")
        {
            AnalyzeWhileStatement(node, state, depth);
            return true;
        }
        if (type == "do_while_statement")
        {
            AnalyzeDoWhileStatement(node, state, depth);
            return true;
        }
        if (type == "for_statement")
        {
            AnalyzeForStatement(node, state, depth);
            return true;
        }
        return false;
    }

    bool AnalyzeJumpStatement(std::string_view type, TSNode node, FlowState& state, int depth)
    {
        if (type == "return_statement")
        {
            if (ts_node_named_child_count(node) > 0)
            {
                CheckExpressionReads(ts_node_named_child(node, 0), state, depth + 1);
            }
            state.hasReturned = true;
            state.isTerminated = true;
            return true;
        }
        if (type == "break_statement" || type == "continue_statement")
        {
            state.isTerminated = true;
            return true;
        }
        return false;
    }

    void AnalyzeStatement(TSNode node, FlowState& state, int depth = 0)
    {
        if (depth > k_maxAstDepth || ts_node_is_null(node) || state.isTerminated || state.hasReturned)
        {
            return;
        }

        std::string_view type = ts_node_type(node);

        if (type == "statement_block")
        {
            AnalyzeBlock(node, state, depth);
            return;
        }

        if (type == "expression_statement")
        {
            if (ts_node_named_child_count(node) > 0)
            {
                CheckExpressionReads(ts_node_named_child(node, 0), state, depth + 1);
            }
            return;
        }

        if (type == "variable_declaration")
        {
            AnalyzeVariableDeclaration(node, state, depth);
            return;
        }

        if (type == "if_statement")
        {
            AnalyzeIfStatement(node, state, depth);
            return;
        }

        if (type == "switch_statement")
        {
            AnalyzeSwitchStatement(node, state, depth);
            return;
        }

        if (AnalyzeLoopStatement(type, node, state, depth) || AnalyzeJumpStatement(type, node, state, depth))
        {
            return;
        }

        CheckExpressionReads(node, state, depth);
    }
};

void TraverseFunctions(TSNode root, DefiniteAssignmentVisitor& visitor)
{
    if (ts_node_is_null(root))
    {
        return;
    }

    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool reachedRoot = false;

    while (!reachedRoot)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        std::string_view type = ts_node_type(node);
        if (type == "func_declaration" || type == "lambda_expression")
        {
            visitor.AnalyzeFunction(node);
        }

        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        while (!reachedRoot)
        {
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                reachedRoot = true;
                break;
            }
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                break;
            }
        }
    }

    ts_tree_cursor_delete(&cursor);
}
} // namespace

void CheckDefiniteAssignment(const DefiniteAssignmentCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(request.root) || request.sourceCode.empty())
    {
        return;
    }

    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    DefiniteAssignmentVisitor visitor(request, ctx);
    if (request.nodeIndex)
    {
        for (TSNode funcNode : request.nodeIndex->Nodes(parser::nodes::FuncDeclaration))
        {
            visitor.AnalyzeFunction(funcNode);
        }
        for (TSNode lambdaNode : request.nodeIndex->Nodes(parser::nodes::LambdaExpression))
        {
            visitor.AnalyzeFunction(lambdaNode);
        }
        return;
    }

    TraverseFunctions(request.root, visitor);
}
} // namespace angel_lsp::analysis
