#pragma once

#include "analysis/index/CallContextFacts.h"
#include "analysis/index/CodeActionFacts.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace angel_lsp::analysis
{

/** @brief A declaration whose single, explicitly initialized variable can receive a leading qualifier. */
struct IndexedLocalConstDeclaration
{
    SourceRange typeRange; ///< Full written type, including any namespace and template.
    IndexedNodeSpan name;  ///< Identifier and its source coordinates for lexical binding.
};

/** @brief A storage use that takes a handle or depends on a callable's const contract. */
struct IndexedLocalConstUse
{
    IndexedNodeSpan storage;        ///< Unqualified identifier at the root of the storage expression.
    IndexedCallContext call;        ///< Callable and receiver spans, empty for address taking.
    IndexedByteRange parameterName; ///< Written parameter name for a named argument.
    uint32_t argumentIndex = 0;     ///< Position of an unnamed argument.
    bool takesAddress = false;      ///< The storage appears under a handle or address operator.
    bool isReceiver = false;        ///< The storage is the receiver of a method call.
};

/** @brief First-pass candidate declarations and uses for a local const qualification edit. */
class LocalConstFacts
{
  public:
    /**
     * @brief Captures initialized declarations and possible reference escapes from ordered syntax.
     * @param[in] index Index of the exact syntax revision.
     */
    explicit LocalConstFacts(const NodeIndex& index);

    /**
     * @brief Looks up a candidate by its type or name span.
     * @param[in] point Zero-based cursor line and byte column.
     * @return The selected declaration, or nullptr outside an eligible type or name.
     * @note Runs in O(log D) for D candidate declarations.
     */
    [[nodiscard]] const IndexedLocalConstDeclaration* FindAt(IndexedSourcePoint point) const noexcept;

    /**
     * @brief Returns storage uses requiring an address or callable const check.
     * @return All captured uses in traversal order, or an empty span when there are none.
     */
    [[nodiscard]] std::span<const IndexedLocalConstUse> Uses() const noexcept
    {
        return m_uses;
    }

    /**
     * @brief Estimates the retained allocation for these facts.
     * @return Bytes occupied by the fact index and its reserved storage.
     */
    [[nodiscard]] size_t EstimateMemoryBytes() const noexcept;

  private:
    std::vector<IndexedLocalConstDeclaration> m_declarations;
    std::vector<IndexedLocalConstUse> m_uses;
};

} // namespace angel_lsp::analysis
