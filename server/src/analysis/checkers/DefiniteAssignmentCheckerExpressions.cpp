#include "analysis/checkers/DefiniteAssignmentCheckerCommon.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::analysis
{

std::string_view DefiniteAssignmentVisitor::SimpleIdentifierName(const IndexedSyntaxNode* operand) const
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
    return operand && operand->symbol == gs.symIdentifier ? parser::NodeText(Syntax(operand), m_request.sourceCode)
                                                          : std::string_view{};
}

void DefiniteAssignmentVisitor::CheckIdentifierRead(const IndexedSyntaxNode* operand,
                                                    const DefiniteAssignmentState& state)
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

bool DefiniteAssignmentVisitor::MarkTrackedAssignment(const IndexedSyntaxNode* operand, DefiniteAssignmentState& state)
{
    std::string_view name = SimpleIdentifierName(operand);
    if (name.empty() || !m_trackedLocals.contains(name))
    {
        return false;
    }
    state.assignedVars.insert(name);
    return true;
}

void DefiniteAssignmentVisitor::QueueRead(const IndexedSyntaxNode* operand, AssignmentReadKind kind)
{
    if (operand)
    {
        m_reads.push_back({operand, kind});
    }
}

void DefiniteAssignmentVisitor::QueueAssignmentReads(const IndexedSyntaxNode* node)
{
    const auto& gs = parser::GetGrammarSymbols();
    const IndexedSyntaxNode* op = FindField(node, gs.fieldOperator);
    QueueRead(FindField(node, gs.fieldLeft),
              op && op->symbol == gs.tokAssign ? AssignmentReadKind::WriteOrRead : AssignmentReadKind::Read);
    QueueRead(FindField(node, gs.fieldRight));
}

void DefiniteAssignmentVisitor::QueueExpressionReads(const IndexedSyntaxNode* node,
                                                     const DefiniteAssignmentState& state)
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

void DefiniteAssignmentVisitor::EvaluateReadTask(AssignmentReadTask task, DefiniteAssignmentState& state)
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

void DefiniteAssignmentVisitor::CheckExpressionReads(const IndexedSyntaxNode* node, DefiniteAssignmentState& state)
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

void DefiniteAssignmentVisitor::AnalyzeVariableDeclarator(const IndexedSyntaxNode* node, bool isPrimitive,
                                                          DefiniteAssignmentState& state)
{
    const auto& gs = parser::GetGrammarSymbols();
    const IndexedSyntaxNode* name = FindFieldOrNamed(node, gs.fieldName);
    if (!name)
    {
        return;
    }

    std::string_view varName = parser::NodeText(Syntax(name), m_request.sourceCode);
    const IndexedSyntaxNode* init = FindField(node, gs.fieldValue);
    if (!init)
    {
        init = FindOperand(node, [](const IndexedSyntaxNode& item) { return item.afterAssign; });
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

void DefiniteAssignmentVisitor::AnalyzeVariableDeclaration(const IndexedSyntaxNode* node,
                                                           DefiniteAssignmentState& state)
{
    const auto& gs = parser::GetGrammarSymbols();
    const IndexedSyntaxNode* type = FindField(node, gs.fieldVarType);
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

} // namespace angel_lsp::analysis
