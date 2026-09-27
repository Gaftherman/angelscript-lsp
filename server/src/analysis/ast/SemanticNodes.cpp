#include "analysis/ast/SemanticNodes.h"

namespace angel_lsp::analysis::ast
{

std::string_view AstNodeView::Text() const noexcept
{
    if (!IsValid() || m_source.empty())
    {
        return {};
    }
    const uint32_t start = ts_node_start_byte(m_raw);
    const uint32_t end = ts_node_end_byte(m_raw);
    if (start <= end && end <= m_source.size())
    {
        return m_source.substr(start, end - start);
    }
    return {};
}

TSNode IfStatementView::Condition() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    TSNode cond = parser::GetChildByField(m_raw, parser::fields::Condition);
    if (ts_node_is_null(cond) && ts_node_named_child_count(m_raw) > 0)
    {
        cond = ts_node_named_child(m_raw, 0);
    }
    return cond;
}

TSNode IfStatementView::Consequence() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    TSNode conseq = parser::GetChildByField(m_raw, parser::fields::Consequence);
    if (ts_node_is_null(conseq) && ts_node_named_child_count(m_raw) > 1)
    {
        conseq = ts_node_named_child(m_raw, 1);
    }
    return conseq;
}

TSNode IfStatementView::Alternative() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    TSNode alt = parser::GetChildByField(m_raw, parser::fields::Alternative);
    if (ts_node_is_null(alt) && ts_node_named_child_count(m_raw) > 2)
    {
        alt = ts_node_named_child(m_raw, 2);
    }
    return alt;
}

bool IfStatementView::HasAlternative() const noexcept
{
    return !ts_node_is_null(Alternative());
}

TSNode ReturnStatementView::Value() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    return parser::GetChildByField(m_raw, parser::fields::Value);
}

bool ReturnStatementView::HasValue() const noexcept
{
    return !ts_node_is_null(Value());
}

TSNode CallExpressionView::Callee() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    TSNode fn = parser::GetChildByField(m_raw, parser::fields::Function);
    if (ts_node_is_null(fn) && ts_node_named_child_count(m_raw) > 0)
    {
        fn = ts_node_named_child(m_raw, 0);
    }
    return fn;
}

std::string_view CallExpressionView::CalleeName() const noexcept
{
    TSNode calleeNode = Callee();
    if (ts_node_is_null(calleeNode))
    {
        return {};
    }
    AstNodeView calleeView(calleeNode, m_source);
    return calleeView.Text();
}

TSNode CallExpressionView::ArgumentList() const noexcept
{
    if (!IsValid())
    {
        return TSNode{};
    }
    const std::string_view nodeType = Type();
    if (nodeType == parser::nodes::ArgumentList)
    {
        return m_raw;
    }
    TSNode args = parser::GetChildByField(m_raw, parser::fields::Arguments);
    if (!ts_node_is_null(args))
    {
        return args;
    }
    const uint32_t count = ts_node_named_child_count(m_raw);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(m_raw, i);
        if (!ts_node_is_null(child) && std::string_view(ts_node_type(child)) == parser::nodes::ArgumentList)
        {
            return child;
        }
    }
    return TSNode{};
}

uint32_t CallExpressionView::ArgumentCount() const noexcept
{
    TSNode argList = ArgumentList();
    if (ts_node_is_null(argList))
    {
        return 0;
    }
    return static_cast<uint32_t>(CountCallArguments(argList));
}

std::vector<TSNode> CallExpressionView::ArgumentNodes() const
{
    TSNode argList = ArgumentList();
    if (ts_node_is_null(argList))
    {
        return {};
    }
    const auto callArgs = ExtractCallArguments(argList, "");
    std::vector<TSNode> nodes;
    nodes.reserve(callArgs.size());
    for (const auto& a : callArgs)
    {
        nodes.push_back(a.exprNode);
    }
    return nodes;
}

std::vector<std::string> CallExpressionView::ArgumentNames() const
{
    TSNode argList = ArgumentList();
    if (ts_node_is_null(argList))
    {
        return {};
    }
    const auto callArgs = ExtractCallArguments(argList, m_source);
    std::vector<std::string> names;
    names.reserve(callArgs.size());
    for (const auto& a : callArgs)
    {
        names.push_back(a.name);
    }
    return names;
}

} // namespace angel_lsp::analysis::ast
