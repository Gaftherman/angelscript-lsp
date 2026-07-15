#pragma once

#include "lsp/types.h"
#include <angelscript.h>
#include <string>
#include <vector>
#include <mutex>

namespace analysis {

// ValidationOracle compiles the given code using the asIScriptEngine 
// (which should have been populated by PredefinedLoader)
// and intercepts any AngelScript compiler messages to translate them to LSP Diagnostics.
class ValidationOracle {
public:
    ValidationOracle(asIScriptEngine* engine);
    ~ValidationOracle();

    // Prevent copy/move because we register this instance as the message callback
    ValidationOracle(const ValidationOracle&) = delete;
    ValidationOracle& operator=(const ValidationOracle&) = delete;

    // Compiles a dummy module to find syntax and semantic errors.
    // NOTE: This uses the AS compiler synchronously. If called from an LSP thread,
    // it will block that thread.
    std::vector<lsp::Diagnostic> ValidateSync(const std::string& code);

private:
    asIScriptEngine* m_engine;
    std::vector<lsp::Diagnostic> m_diagnostics;
    std::mutex m_mutex;

    // The callback provided to AngelScript
    static void MessageCallback(const asSMessageInfo *msg, void *param);
    void HandleMessage(const asSMessageInfo *msg);
};

} // namespace analysis
