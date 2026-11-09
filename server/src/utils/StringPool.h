#pragma once

#include <ankerl/unordered_dense.h>
#include <deque>
#include <string>
#include <string_view>

namespace angel_lsp::utils
{
/**
 * @brief Owns one copy of every distinct text handed to it, so second-pass code can keep zero-copy views of names
 *        whose original storage is temporary. Interned views stay valid until the pool is destroyed.
 * @note Not thread-safe; one pool belongs to one analysis pass on one thread.
 */
class StringPool
{
  public:
    /**
     * @brief Returns a stable view of text, storing a copy only the first time that text is seen.
     * @param[in] text Name to keep; its own storage may be freed right after the call.
     * @return View of the pooled copy; equal texts yield views of the same storage.
     */
    [[nodiscard]] std::string_view Intern(std::string_view text);

  private:
    std::deque<std::string> m_storage;
    ankerl::unordered_dense::set<std::string_view> m_index;
};
} // namespace angel_lsp::utils
