#include "analysis/NullSafetyCondition.h"
#include "analysis/TypeExtraction.h"
#include "parser/GrammarNames.h"
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
    std::string_view op;
    std::string_view sourceCode;
};

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

    if (params.op == "&&" || params.op == "and")
    {
        sink.positive.insert(sink.positive.end(), posL.begin(), posL.end());
        sink.positive.insert(sink.positive.end(), posR.begin(), posR.end());
        sink.negative.insert(sink.negative.end(), negL.begin(), negL.end());
        sink.negative.insert(sink.negative.end(), negR.begin(), negR.end());
    }
    else if (params.op == "||" || params.op == "or")
    {
        sink.negative.insert(sink.negative.end(), negL.begin(), negL.end());
        sink.negative.insert(sink.negative.end(), negR.begin(), negR.end());
    }
}

void ProcessBinaryCondition(TSNode condition, std::string_view sourceCode, AssertionSink& sink)
{
    TSNode left = parser::GetChildByField(condition, parser::fields::Left);
    TSNode opNode = parser::GetChildByField(condition, parser::fields::Operator);
    TSNode right = parser::GetChildByField(condition, parser::fields::Right);
    if (ts_node_is_null(left) || ts_node_is_null(right))
    {
        return;
    }

    std::string op = NodeText(opNode, sourceCode);
    if (op == "is" || op == "==")
    {
        HandleEqualityNullCheck(left, right, sourceCode, sink);
    }
    else if (op == "!is" || op == "!=")
    {
        HandleInequalityNullCheck(left, right, sourceCode, sink);
    }
    else if (op == "&&" || op == "and" || op == "||" || op == "or")
    {
        ProcessLogicalBinary({left, right, op, sourceCode}, sink);
    }
}

void ProcessUnaryCondition(TSNode condition, std::string_view sourceCode, AssertionSink& sink)
{
    TSNode opNode = parser::GetChildByField(condition, parser::fields::Operator);
    TSNode operand = parser::GetChildByField(condition, parser::fields::Operand);
    if (ts_node_is_null(operand))
    {
        return;
    }

    std::string op = NodeText(opNode, sourceCode);
    if (op == "!" || op == "not")
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
    while (!ts_node_is_null(expr))
    {
        std::string_view type = ts_node_type(expr);
        if (type == parser::nodes::ParenthesizedExpression)
        {
            if (ts_node_named_child_count(expr) == 1)
            {
                expr = ts_node_named_child(expr, 0);
                continue;
            }
        }
        else if (type == parser::nodes::UnaryExpression)
        {
            TSNode op = parser::GetChildByField(expr, parser::fields::Operator);
            TSNode operand = parser::GetChildByField(expr, parser::fields::Operand);
            if (!ts_node_is_null(op) && !ts_node_is_null(operand) && ts_node_end_byte(op) - ts_node_start_byte(op) == 1)
            {
                expr = operand;
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
    std::string_view type = ts_node_type(unwrapped);
    if (type == parser::nodes::Identifier)
    {
        return NodeText(unwrapped, sourceCode);
    }
    if (type == parser::nodes::ScopedIdentifier)
    {
        std::string text = NodeText(unwrapped, sourceCode);
        if (text.find("::") == std::string::npos)
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
    std::string_view nodeType = ts_node_type(unwrapped);
    AssertionSink sink{positiveAssertions, negativeAssertions};

    if (nodeType == parser::nodes::BinaryExpression)
    {
        ProcessBinaryCondition(unwrapped, sourceCode, sink);
    }
    else if (nodeType == parser::nodes::UnaryExpression)
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
