#pragma once

#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis::ast
{

/**
 * @brief Base non-owning typed view wrapping a raw Tree-Sitter TSNode and source buffer.
 *
 * Provides safe, zero-allocation AST inspection. Does not take ownership of the
 * underlying tree or source buffer.
 */
class AstNodeView
{
  public:
    /**
     * @brief Constructs an AST node view.
     * @param[in] rawNode Concrete Tree-Sitter syntax node.
     * @param[in] sourceCode Source text buffer corresponding to the node.
     */
    constexpr AstNodeView(TSNode rawNode, std::string_view sourceCode) noexcept : m_raw(rawNode), m_source(sourceCode)
    {
    }

    /** @brief Checks if the node is valid and not null. */
    [[nodiscard]] bool IsValid() const noexcept
    {
        return !ts_node_is_null(m_raw);
    }

    /** @brief Checks if the node is null. */
    [[nodiscard]] bool IsNull() const noexcept
    {
        return ts_node_is_null(m_raw);
    }

    /** @brief Returns the raw grammar type name of the node. */
    [[nodiscard]] std::string_view Type() const noexcept
    {
        return IsValid() ? std::string_view(ts_node_type(m_raw)) : std::string_view{};
    }

    /** @brief Returns the underlying TSNode handle. */
    [[nodiscard]] TSNode Raw() const noexcept
    {
        return m_raw;
    }

    /** @brief Returns the document source buffer. */
    [[nodiscard]] std::string_view Source() const noexcept
    {
        return m_source;
    }

    /** @brief Returns a slice of the source text corresponding to this node. */
    [[nodiscard]] std::string_view Text() const noexcept;

    /** @brief Starting byte offset of the node in the source buffer. */
    [[nodiscard]] uint32_t StartByte() const noexcept
    {
        return IsValid() ? ts_node_start_byte(m_raw) : 0;
    }

    /** @brief Ending byte offset of the node in the source buffer. */
    [[nodiscard]] uint32_t EndByte() const noexcept
    {
        return IsValid() ? ts_node_end_byte(m_raw) : 0;
    }

  protected:
    TSNode m_raw{};
    std::string_view m_source{};
};

/**
 * @brief Typed semantic view for if-statements (`if_statement`).
 */
class IfStatementView : public AstNodeView
{
  public:
    using AstNodeView::AstNodeView;

    /** @brief Evaluates condition AST node. */
    [[nodiscard]] TSNode Condition() const noexcept;

    /** @brief Evaluates then-branch (consequence) statement node. */
    [[nodiscard]] TSNode Consequence() const noexcept;

    /** @brief Evaluates else-branch (alternative) statement node if present. */
    [[nodiscard]] TSNode Alternative() const noexcept;

    /** @brief Checks whether an else-branch is present. */
    [[nodiscard]] bool HasAlternative() const noexcept;
};

/**
 * @brief Typed semantic view for return-statements (`return_statement`).
 */
class ReturnStatementView : public AstNodeView
{
  public:
    using AstNodeView::AstNodeView;

    /** @brief Evaluates the returned expression value node if present. */
    [[nodiscard]] TSNode Value() const noexcept;

    /** @brief Checks whether a return value expression is present. */
    [[nodiscard]] bool HasValue() const noexcept;
};

/**
 * @brief Typed semantic view for call expressions (`call_expression`, `construct_call_expression`).
 */
class CallExpressionView : public AstNodeView
{
  public:
    using AstNodeView::AstNodeView;

    /** @brief Returns the callee expression or function identifier node. */
    [[nodiscard]] TSNode Callee() const noexcept;

    /** @brief Returns the callee identifier name if directly extractable. */
    [[nodiscard]] std::string_view CalleeName() const noexcept;

    /** @brief Returns the argument_list node. */
    [[nodiscard]] TSNode ArgumentList() const noexcept;

    /** @brief Returns the count of arguments passed to the call. */
    [[nodiscard]] uint32_t ArgumentCount() const noexcept;

    /** @brief Extracts all argument expression nodes. */
    [[nodiscard]] std::vector<TSNode> ArgumentNodes() const;

    /** @brief Extracts argument names for each argument (empty if positional). */
    [[nodiscard]] std::vector<std::string> ArgumentNames() const;
};

} // namespace angel_lsp::analysis::ast
