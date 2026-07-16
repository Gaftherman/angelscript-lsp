#pragma once

#include <lsp/types.h>
#include <string>
#include <vector>
#include <shared_mutex>
#include <ankerl/unordered_dense.h>

namespace analysis {

class DiagnosticCache {
public:
    DiagnosticCache() = default;
    ~DiagnosticCache() = default;

    void Update(const std::string& uri, std::vector<lsp::Diagnostic> diags);
    
    std::vector<const lsp::Diagnostic*> GetAt(const std::string& uri, uint32_t line, uint32_t col) const;
    
    void Clear(const std::string& uri);

private:
    mutable std::shared_mutex m_mutex;
    ankerl::unordered_dense::map<std::string, std::vector<lsp::Diagnostic>> m_cache;
};

} // namespace analysis
