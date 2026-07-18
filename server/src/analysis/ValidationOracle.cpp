#include "ValidationOracle.h"
#include "utils/LspLogger.h"
#include "i18n/DiagnosticI18n.h"

namespace analysis
{

ValidationOracle::ValidationOracle(asIScriptEngine *engine, i18n::Locale locale) 
    : m_engine(engine), m_locale(locale)
{
    // Ensure engine has message callback configured initially if needed, 
    // though we override it per ValidateSync call.
}

ValidationOracle::~ValidationOracle()
{
}

std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const std::string &code)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_diagnostics.clear();

    if (!m_engine)
    {
        return m_diagnostics;
    }

    // Temporarily attach our message callback
    m_engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL);

    // Discard any previous module with this name
    const char *moduleName = "ValidationModule";
    m_engine->DiscardModule(moduleName);

    // Create a new module and add the script section
    asIScriptModule *mod = m_engine->GetModule(moduleName, asGM_ALWAYS_CREATE);
    
    if (mod)
    {
        std::string *abstractCode = static_cast<std::string *>(m_engine->GetUserData(2000));
        if (abstractCode && !abstractCode->empty())
        {
            mod->AddScriptSection("Abstracts", abstractCode->c_str(), abstractCode->size());
        }
        
        mod->AddScriptSection("LSP_Doc", code.c_str(), code.size());
        
        // Build the module
        // This will trigger MessageCallback for any syntax or semantic errors.
        int r = mod->Build();
        
        if (r < 0)
        {
            // Compilation failed (diagnostics already populated by MessageCallback)
            angel_lsp::LspLogger::Info("Validation module build returned " + std::to_string(r));
        }
    }

    // Detach message callback to be safe
    m_engine->ClearMessageCallback();
    m_engine->DiscardModule(moduleName);

    return m_diagnostics;
}

void ValidationOracle::MessageCallback(const asSMessageInfo *msg, void *param)
{
    ValidationOracle *oracle = static_cast<ValidationOracle *>(param);
    oracle->HandleMessage(msg);
}

void ValidationOracle::HandleMessage(const asSMessageInfo *msg)
{
    if (msg->section != nullptr && std::string(msg->section) == "Abstracts")
    {
        return; // Ignore any errors generated from our injected abstract classes
    }

    lsp::Diagnostic d;
    
    // AngelScript uses 1-based lines and columns
    d.range.start.line = msg->row > 0 ? msg->row - 1 : 0;
    d.range.start.character = msg->col > 0 ? msg->col - 1 : 0;
    
    // AS doesn't provide end column reliably, so we default to the same line, column + 1
    d.range.end.line = d.range.start.line;
    d.range.end.character = d.range.start.character + 1;

    d.source = "angelscript";
    d.message = i18n::DiagnosticI18n::Translate(msg->message, m_locale);

    switch (msg->type)
    {
        case asMSGTYPE_ERROR:
            d.severity = lsp::DiagnosticSeverity::Error;
            break;
        case asMSGTYPE_WARNING:
            d.severity = lsp::DiagnosticSeverity::Warning;
            break;
        case asMSGTYPE_INFORMATION:
            d.severity = lsp::DiagnosticSeverity::Information;
            break;
    }

    m_diagnostics.push_back(d);
}

} // namespace analysis
