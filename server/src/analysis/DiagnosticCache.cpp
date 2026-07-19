#include "analysis/DiagnosticCache.h"

namespace analysis
{
    void DiagnosticCache::Update(const std::string &uri, std::vector<lsp::Diagnostic> diags)
    {
        std::unique_lock lock(m_mutex);
        m_cache[uri] = std::move(diags);
    }

    std::vector<const lsp::Diagnostic *> DiagnosticCache::GetAt(const std::string &uri, uint32_t line, uint32_t col) const
    {
        std::shared_lock lock(m_mutex);
        std::vector<const lsp::Diagnostic *> results;

        auto it = m_cache.find(uri);
        if (it != m_cache.end())
        {
            for (const auto &d : it->second)
            {
                // AngelScript diagnostics usually have start and end on the same line.
                if (line >= d.range.start.line && line <= d.range.end.line)
                {
                    // In AngelScript diagnostics provided by ValidationOracle,
                    // the range usually spans a single character or starts exactly at the error.
                    // We'll consider it a match if it's on the same line, since AS doesn't provide precise AST ranges.
                    results.push_back(&d);
                }
            }
        }
        return results;
    }

    void DiagnosticCache::Clear(const std::string &uri)
    {
        std::unique_lock lock(m_mutex);
        m_cache.erase(uri);
    }

} // namespace analysis
