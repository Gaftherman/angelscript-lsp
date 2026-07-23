/**
 * @file ValidationOracle.h
 * @brief Pipeline orchestrator for syntax & semantic diagnostic validation rules.
 * @ingroup Analysis
 */

#pragma once

#include "lsp/types.h"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <ankerl/unordered_dense.h>
#include "i18n/LspStrings.h"
#include "document/Document.h"
#include "analysis/SymbolTable.h"
#include "analysis/SymbolResolver.h"
#include "analysis/validation/IValidationRule.h"
#include "analysis/validation/ValidationContext.h"

namespace analysis
{
    /**
     * @brief Pipeline orchestrator for Tree-Sitter syntax & semantic validation rules.
     * @note Thread-safe for synchronous validation via internal mutex locks.
     */
    class ValidationOracle
    {
    public:
        /**
         * @brief Constructs a new ValidationOracle instance and registers pipeline rules.
         *
         * @param[in] locale Target locale for diagnostic translation (defaults to Locale::EN).
         */
        explicit ValidationOracle(i18n::Locale locale = i18n::Locale::EN);

        ~ValidationOracle() = default;

        ValidationOracle(const ValidationOracle &) = delete;
        ValidationOracle &operator=(const ValidationOracle &) = delete;

        /**
         * @brief Synchronously validates a Document and SymbolTables using the rule pipeline.
         *
         * @param[in] doc The Document object containing Tree-Sitter AST and source code.
         * @param[in] localTable Symbol table containing document-local symbols.
         * @param[in] globalTable Symbol table containing predefined global symbols.
         * @param[in] docResolver Optional callback to resolve open documents in memory.
         * @return std::vector<lsp::Diagnostic> A vector of LSP diagnostics belonging to doc.GetUri().
         */
        std::vector<lsp::Diagnostic> ValidateSync(const Document &doc,
                                                   const SymbolTable &localTable,
                                                   const SymbolTable &globalTable,
                                                   std::function<const Document *(const std::string &)> docResolver = nullptr);

        /**
         * @brief Overload for validating a Document with a SymbolResolver instance.
         */
        std::vector<lsp::Diagnostic> ValidateSync(const Document &doc,
                                                   const SymbolTable &symbolTable,
                                                   const SymbolResolver &symbolResolver);

        /**
         * @brief Synchronously validates a raw code string, returning translated LSP diagnostics.
         *
         * @param[in] code The AngelScript source code to validate.
         * @param[in] currentUri Optional document URI for include resolution.
         * @param[in] docResolver Optional callback to resolve open documents in memory.
         * @param[in] globalTable Optional global symbol table.
         * @return std::vector<lsp::Diagnostic> A vector of LSP diagnostics.
         */
        std::vector<lsp::Diagnostic> ValidateSync(std::string_view code,
                                                   std::string_view currentUri = "",
                                                   std::function<const Document *(const std::string &)> docResolver = nullptr,
                                                   const SymbolTable *globalTable = nullptr);

        /**
         * @brief Updates the locale used for diagnostic translation.
         */
        void SetLocale(i18n::Locale locale) { m_locale = locale; }

        /**
         * @brief Updates the set of preprocessor defined words (#if DEFINED / #endif).
         */
        void SetDefinedWords(const std::vector<std::string> &defines);

    private:
        i18n::Locale m_locale;
        ankerl::unordered_dense::set<std::string> m_definedWords = {"DEBUG_MODE"};
        std::mutex m_mutex;

        std::vector<std::unique_ptr<validation::IValidationRule>> m_rules;

        void ValidateIncludeDirectives(const Document &doc,
                                       const std::function<const Document *(const std::string &)> &docResolver,
                                       std::vector<lsp::Diagnostic> &diags);

    };
}
