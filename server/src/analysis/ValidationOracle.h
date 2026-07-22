/**
 * @file ValidationOracle.h
 * @brief Dynamic AngelScript engine compiler message interceptor and diagnostic validator.
 * @ingroup Analysis
 */

#pragma once

#include "lsp/types.h"
#include <angelscript.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <functional>
#include "i18n/LspStrings.h"

#include "document/Document.h"

namespace analysis
{
    /**
     * @brief Compiles AngelScript code dynamically to intercept compiler messages and emit LSP Diagnostics.
     * @note Thread-safe for synchronous validation via internal mutex locks.
     */
    class ValidationOracle
    {
    public:
        /**
         * @brief Constructs a new Validation Oracle instance.
         *
         * @param[in] engine Pointer to the active AngelScript engine.
         * @param[in] locale Target locale for diagnostic translation (defaults to Locale::EN).
         */
        ValidationOracle(asIScriptEngine *engine, i18n::Locale locale = i18n::Locale::EN);

        ~ValidationOracle();

        // Prevent copy/move because we register this instance as the message callback
        ValidationOracle(const ValidationOracle &) = delete;
        ValidationOracle &operator=(const ValidationOracle &) = delete;

        /**
         * @brief Synchronously validates a code string, returning translated LSP diagnostics.
         *
         * @param[in] code The AngelScript source code to validate.
         * @param[in] currentUri Optional document URI for include resolution.
         * @param[in] docResolver Optional callback to resolve open documents in memory.
         * @return std::vector<lsp::Diagnostic> A vector of LSP diagnostics (errors/warnings).
         * @note Thread-safe execution protected by m_mutex.
         */
        std::vector<lsp::Diagnostic> ValidateSync(const std::string &code,
                                                   const std::string &currentUri = "",
                                                   std::function<const Document *(const std::string &)> docResolver = nullptr);

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
        asIScriptEngine *m_engine;
        i18n::Locale m_locale;
        std::unordered_set<std::string> m_definedWords = {"DEBUG_MODE"};
        std::vector<lsp::Diagnostic> m_diagnostics;
        std::mutex m_mutex;

        /**
         * @brief Static C-style callback provided to the AngelScript engine.
         */
        static void MessageCallback(const asSMessageInfo *msg, void *param);

        /**
         * @brief Internal handler that processes and translates raw engine messages.
         */
        void HandleMessage(const asSMessageInfo *msg);
    };
}
