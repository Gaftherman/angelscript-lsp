#pragma once

#include <cstdint>
#include <string_view>
#include <tree_sitter/api.h>

namespace angel_lsp::parser
{
    namespace fields
    {
        inline constexpr std::string_view Object = "object";
        inline constexpr std::string_view Member = "member";
        inline constexpr std::string_view Function = "function";
        inline constexpr std::string_view Arguments = "arguments";
        inline constexpr std::string_view Name = "name";
        inline constexpr std::string_view Parameters = "parameters";
        inline constexpr std::string_view ParamType = "param_type";
        inline constexpr std::string_view ReturnType = "return_type";
        inline constexpr std::string_view Body = "body";
        inline constexpr std::string_view Condition = "condition";
        inline constexpr std::string_view Left = "left";
        inline constexpr std::string_view Right = "right";
        inline constexpr std::string_view Operator = "operator";
        inline constexpr std::string_view Operand = "operand";
        inline constexpr std::string_view Value = "value";
        inline constexpr std::string_view Initializer = "initializer";
        inline constexpr std::string_view Consequence = "consequence";
        inline constexpr std::string_view Alternative = "alternative";
        inline constexpr std::string_view VarType = "var_type";
        inline constexpr std::string_view Type = "type";
    }

    /**
     * @brief Type-safe wrapper for ts_node_child_by_field_name that deduces string length at compile time.
     * @param parent The parent TSNode.
     * @param fieldName The field name string view.
     * @return The child TSNode corresponding to the field name, or a null node if not found.
     */
    [[nodiscard]] inline TSNode GetChildByField(TSNode parent, std::string_view fieldName) noexcept
    {
        if (ts_node_is_null(parent))
        {
            return TSNode{};
        }
        return ts_node_child_by_field_name(parent, fieldName.data(), static_cast<uint32_t>(fieldName.length()));
    }
}
