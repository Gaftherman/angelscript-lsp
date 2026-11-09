#pragma once

#include "analysis/index/NodeIndex.h"

#include <algorithm>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Binary-searches a start-ascending, end-descending fact vector for an exact byte span.
 * @tparam Fact Fact type exposing the span through `bytes`.
 * @param[in] facts Facts ordered by start byte, then by end byte descending.
 * @param[in] bytes Exact span to locate.
 * @return Pointer to the matching fact, or nullptr.
 */
template <typename Fact>
[[nodiscard]] inline const Fact* FindExactSpan(const std::vector<Fact>& facts, IndexedByteRange bytes) noexcept
{
    const auto it = std::lower_bound(facts.begin(), facts.end(), bytes,
                                     [](const Fact& fact, const IndexedByteRange& target)
                                     {
                                         return fact.bytes.startByte != target.startByte
                                                    ? fact.bytes.startByte < target.startByte
                                                    : fact.bytes.endByte > target.endByte;
                                     });
    if (it != facts.end() && it->bytes.startByte == bytes.startByte && it->bytes.endByte == bytes.endByte)
    {
        return &(*it);
    }
    return nullptr;
}

/**
 * @brief Orders facts by start byte ascending, then by end byte descending.
 * @tparam Fact Fact type exposing the span through `bytes`.
 * @param[in,out] facts Vector sorted in place.
 */
template <typename Fact> inline void SortBySpan(std::vector<Fact>& facts)
{
    std::ranges::sort(facts,
                      [](const Fact& a, const Fact& b)
                      {
                          return a.bytes.startByte != b.bytes.startByte ? a.bytes.startByte < b.bytes.startByte
                                                                        : a.bytes.endByte > b.bytes.endByte;
                      });
}

} // namespace angel_lsp::analysis
