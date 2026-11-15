#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/index/ControlFlowFacts.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Primitives.h"

#include <span>
#include <string>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Validates that a switch condition expression evaluates to an integral number or enum.
 * @param[in] record Switch statement record carrying condition spans.
 * @param[in,out] ctx Diagnostic context for type lookup and reporting.
 */
static void CheckSwitchConditionType(const ControlFlowRecord& record, DiagnosticContext& ctx)
{
    if (!record.condition.present || record.hasSyntaxError || !ctx.request.nodeIndex)
    {
        return;
    }

    const Scope* scope = ctx.request.scopeRoot
                             ? FindInnermostScope(ctx.request.scopeRoot.get(), record.condition.range.startLine,
                                                  record.condition.range.startCharacter)
                             : nullptr;
    const std::string rawType = ResolveExpressionType(record.condition.bytes, ExpressionTypeContext(scope, ctx));
    if (rawType.empty())
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const std::string unwrapped = UnwrapTypedef(CleanBaseType(rawType), table);
    if (unwrapped.empty() || unwrapped == "auto" || unwrapped == "?" || ctx.request.IsRegisteredSymbol(unwrapped))
    {
        return;
    }

    if (IsIntegerPrimitive(unwrapped) || ResolvesToEnum(unwrapped, table))
    {
        return;
    }

    const bool known = parser::primitives::IsPrimitive(unwrapped) || unwrapped == "void" ||
                       unwrapped == ctx.request.GetEffectiveStringTypeName() ||
                       unwrapped == ctx.request.GetEffectiveArrayTypeName() ||
                       table.FindSymbolsPtr(unwrapped) != nullptr;
    if (known)
    {
        ctx.EmitAtRange(record.condition.range, diagnostics::codes::SwitchConditionNotIntegral, unwrapped);
    }
}

/**
 * @brief `if (c);` and `else;` - a bare semicolon where the branch body belongs.
 *        AngelScript is stricter here than C++ and stricter than its own loops.
 */
static void CheckEmptyBranch(const ControlFlowRecord& record, DiagnosticContext& ctx)
{
    if (record.branch.hasEmptyConsequence)
    {
        ctx.EmitAtRange(record.branch.consequenceRange, "as-err-if-empty-statement");
    }
    if (record.branch.hasEmptyAlternative)
    {
        ctx.EmitAtRange(record.branch.alternativeRange, "as-err-else-empty-statement");
    }
}

bool ControlFlowVisitor::CheckLoopControl(const ControlFlowRecord& record, const ControlFlowState& state)
{
    if (record.kind == ControlFlowRecordKind::Break)
    {
        if (state.loopDepth == 0 && state.switchDepth == 0)
        {
            m_ctx.EmitAtRange(record.range, "as-err-break-outside-loop");
        }
        return true;
    }
    if (record.kind == ControlFlowRecordKind::Continue)
    {
        if (state.loopDepth == 0)
        {
            m_ctx.EmitAtRange(record.range, "as-err-continue-outside-loop");
        }
        return true;
    }
    return false;
}

void ControlFlowVisitor::UpdateBranchState(const ControlFlowRecord& record, ControlFlowState& state)
{
    if (record.kind == ControlFlowRecordKind::While || record.kind == ControlFlowRecordKind::For ||
        record.kind == ControlFlowRecordKind::Foreach || record.kind == ControlFlowRecordKind::DoWhile)
    {
        ++state.loopDepth;
    }
    else if (record.kind == ControlFlowRecordKind::Switch)
    {
        CheckSwitchConditionType(record, m_ctx);
        CheckSwitchLabels(record, m_facts, m_sourceCode, m_ctx);
        ++state.switchDepth;
    }
    else if (record.kind == ControlFlowRecordKind::If)
    {
        CheckEmptyBranch(record, m_ctx);
    }
}

} // namespace angel_lsp::analysis
