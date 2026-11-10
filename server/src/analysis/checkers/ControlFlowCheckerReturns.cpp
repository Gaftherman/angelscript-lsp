#include "analysis/checkers/ControlFlowCheckerCommon.h"
#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/ControlFlowFacts.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/types/TypeSanitization.h"

#include <optional>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief True for a return type that null cannot convert to.
 */
static bool IsNonNullablePrimitiveName(std::string_view typeName)
{
    return IsPrimitiveTypeName(typeName) && typeName != "void";
}

/**
 * @brief Resolves the syntactic target context of a lambda expression from AST-free declaration and call facts.
 * @param[in] lambdaBytes Byte span of the lambda expression.
 * @param[in] index Precomputed node index for querying declaration and call sites.
 * @param[in] source Document text.
 * @return Populated lambda target fact if an enclosing declarator, assignment, or call argument is found.
 */
static std::optional<LambdaTargetFact> FindLambdaTargetFact(IndexedByteRange lambdaBytes, const NodeIndex& index,
                                                            std::string_view source)
{
    std::optional<LambdaTargetFact> bestTarget;
    uint32_t bestSpanSize = UINT32_MAX;

    const auto declFacts = BuildDeclarationSiteFacts(index, source);
    for (const auto& decl : declFacts.variableDeclarations)
    {
        for (uint32_t i = 0; i < decl.declaratorCount; ++i)
        {
            const auto& d = declFacts.declarators[decl.declaratorBegin + i];
            if (d.hasValue && d.valueOperand.bytes.startByte <= lambdaBytes.startByte &&
                lambdaBytes.endByte <= d.valueOperand.bytes.endByte)
            {
                const uint32_t span = d.valueOperand.bytes.endByte - d.valueOperand.bytes.startByte;
                if (span < bestSpanSize)
                {
                    bestSpanSize = span;
                    bestTarget = LambdaTargetFact{.kind = LambdaTargetKind::DeclaratorType, .text = decl.typeText};
                }
            }
        }
    }

    const auto assignFacts = BuildAssignmentSiteFacts(index, source);
    for (const auto& a : assignFacts.assignments)
    {
        if (a.right.bytes.startByte <= lambdaBytes.startByte && lambdaBytes.endByte <= a.right.bytes.endByte)
        {
            const uint32_t span = a.right.bytes.endByte - a.right.bytes.startByte;
            if (span < bestSpanSize)
            {
                bestSpanSize = span;
                bestTarget = LambdaTargetFact{.kind = LambdaTargetKind::AssignmentLeft, .text = a.left.text};
            }
        }
    }

    const auto callFacts = BuildCallSiteFacts(index, source);
    for (const auto& call : callFacts.callSites)
    {
        for (uint32_t i = 0; i < call.argumentCount; ++i)
        {
            const auto& arg = callFacts.arguments[call.argumentBegin + i];
            if (arg.bytes.startByte <= lambdaBytes.startByte && lambdaBytes.endByte <= arg.bytes.endByte)
            {
                const uint32_t span = arg.bytes.endByte - arg.bytes.startByte;
                if (span < bestSpanSize)
                {
                    bestSpanSize = span;
                    bestTarget = LambdaTargetFact{.kind = LambdaTargetKind::CallArgument,
                                                  .text = call.calleeText,
                                                  .position = i,
                                                  .soleArgument = (call.argumentCount == 1)};
                }
            }
        }
    }

    return bestTarget;
}

void ControlFlowVisitor::CheckReturnStatement(const ControlFlowRecord& record, const ControlFlowState& state)
{
    // A bare `return;` in a function that owes a value.
    if (!state.requiredReturn.empty() && state.requiredReturn != "void" && record.returnFact.isBare)
    {
        m_ctx.EmitAtRange(record.range, "as-err-return-value-required", state.functionName);
    }

    // The constructor half of the same rule.
    if (state.implicitVoid && record.returnFact.hasValue)
    {
        m_ctx.EmitAtRange(record.range, "as-err-void-return-value");
    }

    // `int f() { return null; }` - "No conversion from '<null handle>' to 'int' available."
    if (!state.requiredReturn.empty() && !IsHandleTypeView(state.requiredReturn) &&
        IsNonNullablePrimitiveName(state.requiredReturn) && record.returnFact.returnsNullLiteral)
    {
        m_ctx.EmitAtRange(record.range, "as-err-null-non-handle", state.requiredReturn);
    }
}

void ControlFlowVisitor::CheckFunctionDeclaration(const ControlFlowRecord& record, ControlFlowState& state)
{
    // A nested function opens its own flow: a loop enclosing the declaration does not
    // make a `break` inside the nested body legal.
    state = ControlFlowState{};

    const ControlFlowFunctionFact* fnFact = m_facts.FindFunction(record.bytes);
    if (!fnFact)
    {
        return;
    }

    std::string_view requiredReturn;
    std::string_view reportedName;
    if (!fnFact->isLambda)
    {
        requiredReturn = fnFact->returnType;
        reportedName = fnFact->name;
    }
    else
    {
        if (m_ctx.request.nodeIndex != nullptr)
        {
            if (const auto ltf = FindLambdaTargetFact(fnFact->bytes, *m_ctx.request.nodeIndex, m_sourceCode))
            {
                if (const auto target = FuncdefTargetOfLambda(*ltf, m_ctx.request.symbolTable))
                {
                    const std::string_view retType = CleanBaseTypeView(target->GetFuncdef().returnType);
                    if (retType == "void")
                    {
                        requiredReturn = "void";
                    }
                    else
                    {
                        m_lambdaStrings.emplace_back(retType);
                        requiredReturn = m_lambdaStrings.back();
                    }
                    m_lambdaStrings.emplace_back(target->name);
                    reportedName = m_lambdaStrings.back();
                }
            }
        }

        if (requiredReturn.empty())
        {
            // For lambdas, search return sites belonging to this lambda to find the funcdef target
            for (const auto& ret : m_facts.returnSites.returns)
            {
                if (ret.owner == ReturnOwnerKind::Lambda && ret.bytes.startByte >= fnFact->bytes.startByte &&
                    ret.bytes.endByte <= fnFact->bytes.endByte)
                {
                    if (const auto target = FuncdefTargetOfLambda(ret.lambdaTarget, m_ctx.request.symbolTable))
                    {
                        const std::string_view retType = CleanBaseTypeView(target->GetFuncdef().returnType);
                        if (retType == "void")
                        {
                            requiredReturn = "void";
                        }
                        else
                        {
                            m_lambdaStrings.emplace_back(retType);
                            requiredReturn = m_lambdaStrings.back();
                        }
                        m_lambdaStrings.emplace_back(target->name);
                        reportedName = m_lambdaStrings.back();
                        break;
                    }
                }
            }
        }
    }

    if (fnFact->bodyBytes.startByte != fnFact->bodyBytes.endByte && !requiredReturn.empty() && requiredReturn != "void")
    {
        if (!fnFact->hasSyntaxError && !fnFact->definitelyReturns)
        {
            const SourceRange anchor = fnFact->isLambda || fnFact->name.empty() ? fnFact->range : fnFact->nameRange;
            m_ctx.EmitAtRange(anchor, "as-err-not-all-paths-return", reportedName);
        }
    }

    state.requiredReturn = requiredReturn;
    state.functionName = reportedName;
    state.implicitVoid = fnFact->implicitVoid;
}

} // namespace angel_lsp::analysis
