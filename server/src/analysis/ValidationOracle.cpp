#include "ValidationOracle.h"
#include "utils/LspLogger.h"
#include "i18n/DiagnosticI18n.h"
#include "analysis/SymbolCollector.h"
#include "document/Document.h"
#include <sstream>
#include <fstream>
#include <unordered_set>

static std::string SanitizeCodeForEngine(const std::string &code)
{
    std::string sanitizedCode;
    sanitizedCode.reserve(code.size());
    std::stringstream ss(code);
    std::string line;
    bool first = true;

    int ifDepth = 0;
    bool inInactiveElseBranch = false;

    while (std::getline(ss, line))
    {
        if (!first) sanitizedCode += "\n";
        first = false;

        size_t firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '#')
        {
            std::string prefix = line.substr(0, firstNonSpace);
            std::string directive = line.substr(firstNonSpace);

            if (directive.starts_with("#else") || directive.starts_with("#elif"))
            {
                inInactiveElseBranch = true;
            }
            else if (directive.starts_with("#endif"))
            {
                if (ifDepth > 0) ifDepth--;
                if (ifDepth == 0) inInactiveElseBranch = false;
            }
            else if (directive.starts_with("#if"))
            {
                ifDepth++;
            }

            sanitizedCode += prefix + "// " + line.substr(firstNonSpace + 1);
        }
        else if (inInactiveElseBranch)
        {
            size_t indent = (firstNonSpace != std::string::npos) ? firstNonSpace : 0;
            sanitizedCode += line.substr(0, indent) + "// " + line.substr(indent);
        }
        else
        {
            sanitizedCode += line;
        }
    }
    return sanitizedCode;
}

namespace analysis
{

    ValidationOracle::ValidationOracle(asIScriptEngine *engine, i18n::Locale locale)
        : m_engine(engine), m_locale(locale)
    {
    }

    ValidationOracle::~ValidationOracle()
    {
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const std::string &code,
                                                               const std::string &currentUri,
                                                               std::function<const Document *(const std::string &)> docResolver)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnostics.clear();

        if (!m_engine)
        {
            return m_diagnostics;
        }

        m_engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL);

        const char *moduleName = "ValidationModule";
        m_engine->DiscardModule(moduleName);

        asIScriptModule *mod = m_engine->GetModule(moduleName, asGM_ALWAYS_CREATE);

        if (mod)
        {
            std::string *abstractCode = static_cast<std::string *>(m_engine->GetUserData(2000));
            if (abstractCode && !abstractCode->empty())
            {
                mod->AddScriptSection("Abstracts", abstractCode->c_str(), abstractCode->size());
            }

            if (!currentUri.empty())
            {
                std::unordered_set<std::string> visited;
                visited.insert(currentUri);

                std::function<void(const std::string &, const std::string &)> loadIncludes =
                    [&](const std::string &baseUri, const std::string &srcCode) {
                        std::stringstream ss(srcCode);
                        std::string line;
                        while (std::getline(ss, line))
                        {
                            size_t firstNonSpace = line.find_first_not_of(" \t\r");
                            if (firstNonSpace != std::string::npos && line.substr(firstNonSpace).starts_with("#include"))
                            {
                                std::string relPath = SymbolCollector::ExtractIncludePath(line.substr(firstNonSpace));
                                if (!relPath.empty())
                                {
                                    std::string targetUri = SymbolCollector::ResolveIncludeUri(baseUri, relPath);
                                    if (!targetUri.empty() && visited.insert(targetUri).second)
                                    {
                                        std::string incContent;
                                        const Document *openDoc = docResolver ? docResolver(targetUri) : nullptr;
                                        if (openDoc)
                                        {
                                            incContent = openDoc->GetText();
                                        }
                                        else
                                        {
                                            std::string filePath = targetUri;
                                            if (filePath.starts_with("file:///")) filePath = filePath.substr(8);
                                            else if (filePath.starts_with("file://")) filePath = filePath.substr(7);

                                            std::ifstream infile(filePath);
                                            if (infile.is_open())
                                            {
                                                std::stringstream buf;
                                                buf << infile.rdbuf();
                                                incContent = buf.str();
                                            }
                                        }

                                        if (!incContent.empty())
                                        {
                                            loadIncludes(targetUri, incContent);
                                            std::string sanitizedInc = SanitizeCodeForEngine(incContent);
                                            mod->AddScriptSection(targetUri.c_str(), sanitizedInc.c_str(), sanitizedInc.size());
                                        }
                                    }
                                }
                            }
                        }
                    };

                loadIncludes(currentUri, code);
            }

            std::string sanitizedMain = SanitizeCodeForEngine(code);
            mod->AddScriptSection("LSP_Doc", sanitizedMain.c_str(), sanitizedMain.size());

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
