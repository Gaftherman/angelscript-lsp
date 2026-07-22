/**
 * @file ValidationOracle.h
 * @brief Pure Tree-Sitter syntax & semantic diagnostic validator.
 * @ingroup Analysis
 */

#pragma once

#include "lsp/types.h"
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <functional>
#include "i18n/LspStrings.h"
#include "document/Document.h"
#include "analysis/SymbolTable.h"

namespace analysis
{
    /**
     * @brief Dual-pass Tree-Sitter syntax & semantic diagnostic validator.
     * @note Thread-safe for synchronous validation via internal mutex locks.
     */
    class ValidationOracle
    {
    public:
        /**
         * @brief Constructs a new Validation Oracle instance.
         *
         * @param[in] locale Target locale for diagnostic translation (defaults to Locale::EN).
         */
        explicit ValidationOracle(i18n::Locale locale = i18n::Locale::EN);

        ~ValidationOracle();

        ValidationOracle(const ValidationOracle &) = delete;
        ValidationOracle &operator=(const ValidationOracle &) = delete;

        /**
         * @brief Synchronously validates a Document and SymbolTables, returning translated LSP diagnostics.
         *
         * @param[in] doc The Document object containing Tree-Sitter AST and source code.
         * @param[in] localTable Symbol table containing document-local symbols.
         * @param[in] globalTable Symbol table containing predefined global symbols.
         * @param[in] docResolver Optional callback to resolve open documents in memory.
         * @return std::vector<lsp::Diagnostic> A vector of LSP diagnostics belonging strictly to doc.GetUri().
         */
        std::vector<lsp::Diagnostic> ValidateSync(const Document &doc,
                                                   const SymbolTable &localTable,
                                                   const SymbolTable &globalTable,
                                                   std::function<const Document *(const std::string &)> docResolver = nullptr);

        /**
         * @brief Synchronously validates a raw code string, returning translated LSP diagnostics.
         *
         * @param[in] code The AngelScript source code to validate.
         * @param[in] currentUri Optional document URI for include resolution.
         * @param[in] docResolver Optional callback to resolve open documents in memory.
         * @param[in] globalTable Optional global symbol table.
         * @return std::vector<lsp::Diagnostic> A vector of LSP diagnostics.
         */
        std::vector<lsp::Diagnostic> ValidateSync(const std::string &code,
                                                   const std::string &currentUri = "",
                                                   std::function<const Document *(const std::string &)> docResolver = nullptr,
                                                   const SymbolTable *globalTable = nullptr);

        /**
         * @brief Updates the locale used for diagnostic translation.
         *
         * @param[in] locale The new Locale enum value.
         */
        void SetLocale(i18n::Locale locale) { m_locale = locale; }

        /**
         * @brief Updates the set of preprocessor defined words (#if DEFINED / #endif).
         *
         * @param[in] defines List of defined preprocessor tokens.
         */
        void SetDefinedWords(const std::vector<std::string> &defines);

    private:
        i18n::Locale m_locale;
        std::unordered_set<std::string> m_definedWords = {"DEBUG_MODE"};
        std::mutex m_mutex;

        /**
         * @brief Validates #include preprocessor directives syntax and existence.
         */
        void ValidateIncludeDirective(const Document &doc,
                                       const std::function<const Document *(const std::string &)> &docResolver,
                                       std::vector<lsp::Diagnostic> &diags);

        /**
         * @brief Collects AST syntax errors (TSNode IS_ERROR / IS_MISSING).
         */
        void CollectSyntaxErrors(const Document &doc, std::vector<lsp::Diagnostic> &diags);

        /**
         * @brief Collects semantic errors (unresolvable identifiers/types/functions).
         */
        void CollectSemanticErrors(const Document &doc, const SymbolTable &localTable, const SymbolTable &globalTable, std::vector<lsp::Diagnostic> &diags);
    };
}
