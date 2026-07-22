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
     * @brief A cache for storing diagnostics (errors/warnings) emitted by the compiler.
     */
    class DiagnosticCache
    {
    public:
        DiagnosticCache() = default;
        ~DiagnosticCache() = default;

        /**
         * @brief Updates the diagnostics for a specific file URI.
         *
         * @param uri The URI of the file.
         * @param diags The new list of diagnostics for the file.
         */
        void Update(const std::string &uri, std::vector<lsp::Diagnostic> diags);

        /**
         * @brief Retrieves all diagnostics that intersect the given line and column in a file.
         *
         * @param uri The URI of the file.
         * @param line The 0-based line number.
         * @param col The 0-based column number.
         * @return A vector of pointers to the matching diagnostics.
         */
        std::vector<const lsp::Diagnostic *> GetAt(const std::string &uri, uint32_t line, uint32_t col) const;

        /**
         * @brief Clears all diagnostics for a specific file URI.
         *
         * @param uri The URI of the file.
         */
        void Clear(const std::string &uri);

    private:
        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::vector<lsp::Diagnostic>> m_cache;
    };

} // namespace analysis
