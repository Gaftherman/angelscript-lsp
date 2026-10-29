#include "analysis/NullSafetyCondition.h"
#include "analysis/ASTUtils.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"
#include <string>

namespace angel_lsp::analysis
{
namespace
{
struct AssertionSink
{
    std::vector<NullAssertion>& positive;
    std::vector<NullAssertion>& negative;
};

struct BinaryConditionParams
{
    TSNode left;
    TSNode right;
    TSSymbol opSym;
    std::string_view sourceCode;
};

bool HandleEqualityNullCheck(TSNode left, TSNode right, std::string_view sourceCode, AssertionSink& sink)
{
    std::string name;
    if (IsNullInitializer(right))
    {
        name = GetIdentifierName(left, sourceCode);
    }
    else if (IsNullInitializer(left))
    {
        name = GetIdentifierName(right, sourceCode);
    }

    if (!name.empty())
    {
        sink.positive.push_back({name, Nullability::DefinitelyNull});
        sink.negative.push_back({name, Nullability::NonNull});
        return true;
    }
    return false;
}

bool HandleInequalityNullCheck(TSNode left, TSNode right, std::string_view sourceCode, AssertionSink& sink)
{
    std::string name;
    if (IsNullInitializer(right))
    {
        name = GetIdentifierName(left, sourceCode);
    }
    else if (IsNullInitializer(left))
    {
        name = GetIdentifierName(right, sourceCode);
    }

    if (!name.empty())
    {
        sink.positive.push_back({name, Nullability::NonNull});
        sink.negative.push_back({name, Nullability::DefinitelyNull});
        return true;
    }
    return false;
}

void ProcessLogicalBinary(const BinaryConditionParams& params, AssertionSink& sink)
{
    std::vector<NullAssertion> posL;
    std::vector<NullAssertion> negL;
    std::vector<NullAssertion> posR;
    std::vector<NullAssertion> negR;

    ExtractConditionAssertions(params.left, params.sourceCode, posL, negL);
    ExtractConditionAssertions(params.right, params.sourceCode, posR, negR);

    const auto& gs = parser::GetGrammarSymbols();
    if (params.opSym == gs.tokAmpAmp || params.opSym == gs.tokAnd)
    {
        sink.positive.insert(sink.positive.end(), posL.begin(), posL.end());
        sink.positive.insert(sink.positive.end(), posR.begin(), posR.end());
        sink.negative.insert(sink.negative.end(), negL.begin(), negL.end());
        sink.negative.insert(sink.negative.end(), negR.begin(), negR.end());
    }
    else if (params.opSym == gs.tokPipePipe || params.opSym == gs.tokOr)
    {
        for (const auto& l : posL)
        {
            for (const auto& r : posR)
            {
                if (l.varName == r.varName && l.state == r.state)
                {
                    sink.positive.push_back(l);
                    break;
                }
            }
        }
        sink.negative.insert(sink.negative.end(), negL.begin(), negL.end());
        sink.negative.insert(sink.negative.end(), negR.begin(), negR.end());
    }
}

void ProcessBinaryCondition(TSNode condition, std::string_view sourceCode, AssertionSink& sink)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode left = parser::GetChildByField(condition, gs.fieldLeft);
    TSNode opNode = parser::GetChildByField(condition, gs.fieldOperator);
    TSNode right = parser::GetChildByField(condition, gs.fieldRight);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym == gs.tokIs || opSym == gs.tokEqEq)
    {
        HandleEqualityNullCheck(left, right, sourceCode, sink);
    }
    else if (opSym == gs.tokBangIs || opSym == gs.tokBangEq)
    {
        HandleInequalityNullCheck(left, right, sourceCode, sink);
    }
    else if (opSym == gs.tokAmpAmp || opSym == gs.tokAnd || opSym == gs.tokPipePipe || opSym == gs.tokOr)
    {
        ProcessLogicalBinary({left, right, opSym, sourceCode}, sink);
    }
}

void ProcessUnaryCondition(TSNode condition, std::string_view sourceCode, AssertionSink& sink)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode opNode = parser::GetChildByField(condition, gs.fieldOperator);
    TSNode operand = parser::GetChildByField(condition, gs.fieldOperand);
    if (ts_node_is_null(operand))
    {
        return;
    }

    const TSSymbol opSym = ts_node_symbol(opNode);
    if (opSym == gs.tokBang || opSym == gs.tokNot)
    {
        std::vector<NullAssertion> innerPos;
        std::vector<NullAssertion> innerNeg;
        ExtractConditionAssertions(operand, sourceCode, innerPos, innerNeg);
        sink.positive.insert(sink.positive.end(), innerNeg.begin(), innerNeg.end());
        sink.negative.insert(sink.negative.end(), innerPos.begin(), innerPos.end());
    }
}
} // namespace

TSNode UnwrapNullExpression(TSNode expr)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(expr))
    {
        const TSSymbol sym = ts_node_symbol(expr);
        if (sym == gs.symParenthesizedExpression)
        {
            if (ts_node_named_child_count(expr) == 1)
            {
                expr = ts_node_named_child(expr, 0);
                continue;
            }
        }
        else if (sym == gs.symUnaryExpression)
        {
            TSNode op = parser::GetChildByField(expr, gs.fieldOperator);
            TSNode operand = parser::GetChildByField(expr, gs.fieldOperand);
            if (!ts_node_is_null(op) && !ts_node_is_null(operand) && ts_node_end_byte(op) - ts_node_start_byte(op) == 1)
            {
                expr = operand;
                continue;
            }
        }
        else if (sym == gs.symAssignmentExpression)
        {
            TSNode left = parser::GetChildByField(expr, gs.fieldLeft);
            if (!ts_node_is_null(left))
            {
                expr = left;
                continue;
            }
        }
        break;
    }
    return expr;
}

std::string GetIdentifierName(TSNode node, std::string_view sourceCode)
{
    TSNode unwrapped = UnwrapNullExpression(node);
    if (ts_node_is_null(unwrapped))
    {
        return "";
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(unwrapped);
    if (sym == gs.symIdentifier)
    {
        return std::string(NodeText(unwrapped, sourceCode));
    }
    if (sym == gs.symScopedIdentifier)
    {
        std::string text = NodeText(unwrapped, sourceCode);
        if (!utils::ContainsScopeSeparator(text))
        {
            return text;
        }
    }
    return "";
}

void ExtractConditionAssertions(TSNode condition, std::string_view sourceCode,
                                std::vector<NullAssertion>& positiveAssertions,
                                std::vector<NullAssertion>& negativeAssertions)
{
    if (ts_node_is_null(condition))
    {
        return;
    }

    TSNode unwrapped = UnwrapNullExpression(condition);
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(unwrapped);
    AssertionSink sink{positiveAssertions, negativeAssertions};

    if (sym == gs.symBinaryExpression)
    {
        ProcessBinaryCondition(unwrapped, sourceCode, sink);
    }
    else if (sym == gs.symUnaryExpression)
    {
        ProcessUnaryCondition(unwrapped, sourceCode, sink);
    }
    else
    {
        std::string name = GetIdentifierName(unwrapped, sourceCode);
        if (!name.empty())
        {
            sink.positive.push_back({name, Nullability::NonNull});
            sink.negative.push_back({name, Nullability::DefinitelyNull});
        }
    }
}

} // namespace angel_lsp::analysis
