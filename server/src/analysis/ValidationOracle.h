#pragma once

#include "lsp/types.h"
#include <angelscript.h>
#include <string>
#include <vector>
#include <mutex>
#include "i18n/LspStrings.h"

namespace analysis
{
    /**
     * @brief Compiles AngelScript code dynamically to intercept compiler messages.
     * 
     * Uses the asIScriptEngine (populated by PredefinedLoader) to compile a 
     * temporary module and intercepts any AngelScript syntax or semantic errors
     * to translate them into LSP Diagnostics.
     */
    class ValidationOracle
    {
    public:
        /**
         * @brief Constructs a new Validation Oracle.
         * 
         * @param engine Pointer to the active AngelScript engine.
         * @param locale The locale used for diagnostic translation.
         */
        ValidationOracle(asIScriptEngine *engine, i18n::Locale locale = i18n::Locale::EN);
        
        ~ValidationOracle();

        // Prevent copy/move because we register this instance as the message callback
        ValidationOracle(const ValidationOracle &) = delete;
        ValidationOracle &operator=(const ValidationOracle &) = delete;

        /**
         * @brief Synchronously validates a code string, returning diagnostics.
         * 
         * @param code The AngelScript source code to validate.
         * @return A list of LSP diagnostics (errors/warnings).
         */
        std::vector<lsp::Diagnostic> ValidateSync(const std::string &code);
        
        /**
         * @brief Updates the locale used for diagnostic translation.
         * 
         * @param locale The new locale.
         */
        void SetLocale(i18n::Locale locale) { m_locale = locale; }

    private:
        asIScriptEngine *m_engine;
        i18n::Locale m_locale;
        std::vector<lsp::Diagnostic> m_diagnostics;
        std::mutex m_mutex;

        /**
         * @brief Static callback provided to the AngelScript engine.
         */
        static void MessageCallback(const asSMessageInfo *msg, void *param);
        
        /**
         * @brief Internal handler that processes and translates the raw engine message.
         */
        void HandleMessage(const asSMessageInfo *msg);
    };
}
