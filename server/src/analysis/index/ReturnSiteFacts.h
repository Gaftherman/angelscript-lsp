#pragma once

#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/LambdaTargetFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/Symbol.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Kind of callable that owns a return statement.
 */
enum class ReturnOwnerKind : uint8_t
{
    None,    ///< No enclosing function or lambda was indexed; the statement is skipped.
    Lambda,  ///< Return inside a lambda expression.
    Function ///< Return inside a function declaration.
};

/**
 * @brief A return statement that carries a value, with the owner data the return checks read.
 */
struct ReturnSiteFact
{
    IndexedByteRange bytes;     ///< Byte span of the return statement.
    SourceRange statementRange; ///< Line and column span; its start selects the lexical scope.
    BinaryOperandFact value;    ///< Returned expression with its spelling and constant-folding data.
    ReturnOwnerKind owner = ReturnOwnerKind::None;
    LambdaTargetFact lambdaTarget; ///< Context of the owning lambda; meaningful when owner is Lambda.
    std::string_view returnType;   ///< Declared return type of the owning function; empty when none was written.
    SourceRange ownerRange; ///< Span of the owning function; bounds locals that may not be returned by reference.
    bool returnsReference = false; ///< The owning function returns by reference.
};

/**
 * @brief AST-free return statements of one revision.
 * @note Views point into the source passed to BuildReturnSiteFacts, which must outlive this object.
 */
struct ReturnSiteFacts
{
    std::vector<ReturnSiteFact> returns; ///< Ordered by start byte, then by end byte descending.

    /**
     * @brief Finds the return statement that spans exactly the given bytes.
     * @param[in] bytes Byte span of the return statement.
     * @return The fact, or nullptr when the span is not a return statement with a value.
     * @note Cost O(log N) binary search.
     */
    [[nodiscard]] const ReturnSiteFact* Find(IndexedByteRange bytes) const noexcept;
};

/**
 * @brief Extracts every valued return statement of the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document text; must outlive the returned facts.
 * @return Facts ordered by start byte.
 */
[[nodiscard]] ReturnSiteFacts BuildReturnSiteFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
