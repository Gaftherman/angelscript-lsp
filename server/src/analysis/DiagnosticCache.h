/**
 * @file DiagnosticCache.h
 * @brief Thread-safe diagnostic cache container protected by shared_mutex.
 * @ingroup Analysis
 */

#pragma once

#include <lsp/types.h>
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <ankerl/unordered_dense.h>

namespace analysis
{
    /**
     * @brief Thread-safe cache for storing and querying LSP diagnostics per document URI.
     * @note Thread-safe for concurrent read operations (GetAt) and write operations (Update, Clear) via m_mutex.
     */
    class DiagnosticCache
    {
    public:
        DiagnosticCache() = default;
        ~DiagnosticCache() = default;

        /**
         * @brief Updates the stored diagnostics vector for a specific document URI.
         *
         * @param[in] uri The document URI string.
         * @param[in] diags Vector of LSP diagnostics to store.
         * @note Thread-safe write method protected by unique_lock.
         */
        void Update(const std::string &uri, std::vector<lsp::Diagnostic> diags);

        /**
         * @brief Retrieves all stored diagnostics intersecting a line and column position.
         *
         * @param[in] uri The document URI string.
         * @param[in] line Zero-indexed line number.
         * @param[in] col Zero-indexed column number.
         * @return std::vector<const lsp::Diagnostic*> Vector of pointers to matching diagnostics.
         * @note Thread-safe read method protected by shared_lock.
         */
        std::vector<const lsp::Diagnostic *> GetAt(const std::string &uri, uint32_t line, uint32_t col) const;

        /**
         * @brief Clears all stored diagnostics for a specific file URI.
         *
         * @param[in] uri The document URI string.
         * @note Thread-safe write method protected by unique_lock.
         */
        void Clear(const std::string &uri);

    private:
        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::vector<lsp::Diagnostic>> m_cache;
    };

} // namespace analysis
