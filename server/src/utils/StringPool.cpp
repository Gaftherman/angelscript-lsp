#include "utils/StringPool.h"

namespace angel_lsp::utils
{
std::string_view StringPool::Intern(std::string_view text)
{
    if (const auto it = m_index.find(text); it != m_index.end())
    {
        return *it;
    }
    const std::string_view pooled = m_storage.emplace_back(text);
    m_index.insert(pooled);
    return pooled;
}
} // namespace angel_lsp::utils
