/**
 * @file ValidationOracle.cpp
 * @brief Implementation of ValidationOracle dynamic compiler error message translation and include validation.
 * @ingroup Analysis
 */

#include "ValidationOracle.h"
#include "utils/LspLogger.h"
#include "i18n/DiagnosticI18n.h"
#include "analysis/SymbolCollector.h"
#include "document/Document.h"
#include <sstream>
#include <fstream>
#include <unordered_set>
#include <algorithm>

static std::string UrlDecode(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '%' && i + 2 < in.size())
        {
            int hexVal = 0;
            std::stringstream ss;
            ss << std::hex << in.substr(i + 1, 2);
            if (ss >> hexVal)
            {
                out += static_cast<char>(hexVal);
                i += 2;
                continue;
            }
        }
        out += in[i];
    }
    return out;
}

static std::string NormalizeUri(const std::string &rawUri)
{
    if (rawUri.empty()) return "";

    std::string out;
    out.reserve(rawUri.size());

    // URL decode %XX
    for (size_t i = 0; i < rawUri.size(); ++i)
    {
        if (rawUri[i] == '%' && i + 2 < rawUri.size())
        {
            int hexVal = 0;
            std::stringstream ss;
            ss << std::hex << rawUri.substr(i + 1, 2);
            if (ss >> hexVal)
            {
                out += static_cast<char>(hexVal);
                i += 2;
                continue;
            }
        }
        out += rawUri[i];
    }

    // Standardize slashes: replace \ with /
    std::replace(out.begin(), out.end(), '\\', '/');

    // Normalize drive letter casing e.g. file:///c:/ -> file:///c:/
    if (out.starts_with("file:///"))
    {
        if (out.size() >= 10 && std::isalpha(static_cast<unsigned char>(out[8])) && out[9] == ':')
        {
            out[8] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[8])));
        }
    }

    return out;
}

static std::string SanitizeCodeForEngine(const std::string &code, const std::unordered_set<std::string> &definedWords)
{
    std::string sanitizedCode;
    sanitizedCode.reserve(code.size());

    std::stringstream ss(code);
    std::string line;
    bool first = true;

    std::vector<bool> ifStack;

    auto currentActive = [&ifStack]() -> bool
    {
        for (bool active : ifStack)
        {
            if (!active)
            {
                return false;
            }
        }
        return true;
    };

    while (std::getline(ss, line))
    {
        if (!first) sanitizedCode += "\n";
        first = false;

        size_t firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace != std::string::npos && line[firstNonSpace] == '#')
        {
            std::string directiveLine = line.substr(firstNonSpace + 1);
            std::stringstream lineSs(directiveLine);
            std::string directive;
            lineSs >> directive;

            std::string prefix = line.substr(0, firstNonSpace);

            if (directive == "if")
            {
                std::string word;
                lineSs >> word;
                bool isNegated = false;
                if (word == "!" && lineSs >> word)
                {
                    isNegated = true;
                }
                else if (word.starts_with("!"))
                {
                    isNegated = true;
                    word = word.substr(1);
                }

                bool exists = definedWords.contains(word);
                bool cond = isNegated ? !exists : exists;
                ifStack.push_back(cond);

                sanitizedCode += prefix + "// " + line.substr(firstNonSpace + 1);
                continue;
            }
            else if (directive == "endif")
            {
                if (!ifStack.empty())
                {
                    ifStack.pop_back();
                }
                sanitizedCode += prefix + "// " + line.substr(firstNonSpace + 1);
                continue;
            }
            else
            {
                // Other directive e.g. #include, #pragma
                sanitizedCode += prefix + "// " + line.substr(firstNonSpace + 1);
                continue;
            }
        }

        // Regular code line
        if (currentActive())
        {
            sanitizedCode += line;
        }
        else
        {
            size_t start = line.find_first_not_of(" \t\r");
            if (start != std::string::npos)
            {
                sanitizedCode += line.substr(0, start) + "// " + line.substr(start);
            }
            else
            {
                sanitizedCode += line;
            }
        }
    }
    return sanitizedCode;
}

static bool ValidateIncludeDirective(const std::string &line,
                                     size_t lineIdx,
                                     size_t firstNonSpace,
                                     const std::string &baseUri,
                                     std::function<const Document *(const std::string &)> docResolver,
                                     angel_lsp::i18n::Locale locale,
                                     std::vector<lsp::Diagnostic> &diagsOut)
{
    size_t hashPos = line.find('#', firstNonSpace);
    if (hashPos == std::string::npos)
    {
        hashPos = firstNonSpace;
    }

    size_t openQuote = line.find_first_of("\"<", hashPos);
    if (openQuote == std::string::npos)
    {
        lsp::Diagnostic d;
        d.range.start.line = lineIdx;
        d.range.start.character = hashPos;
        d.range.end.line = lineIdx;
        d.range.end.character = line.length();
        d.severity = lsp::DiagnosticSeverity::Error;
        d.source = "angelscript";
        d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Invalid #include directive: missing opening quote or angle bracket", locale);
        diagsOut.push_back(d);
        return false;
    }

    char closeChar = (line[openQuote] == '<') ? '>' : '"';
    size_t closeQuote = line.find(closeChar, openQuote + 1);
    if (closeQuote == std::string::npos)
    {
        lsp::Diagnostic d;
        d.range.start.line = lineIdx;
        d.range.start.character = hashPos;
        d.range.end.line = lineIdx;
        d.range.end.character = line.length();
        d.severity = lsp::DiagnosticSeverity::Error;
        d.source = "angelscript";
        d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Invalid #include directive: unclosed path delimiter ('" + std::string(1, closeChar) + "')", locale);
        diagsOut.push_back(d);
        return false;
    }

    size_t trailingPos = closeQuote + 1;
    while (trailingPos < line.length() && (line[trailingPos] == ' ' || line[trailingPos] == '\t' || line[trailingPos] == '\r' || line[trailingPos] == '\n'))
    {
        trailingPos++;
    }

    if (trailingPos < line.length())
    {
        lsp::Diagnostic d;
        d.range.start.line = lineIdx;
        d.range.start.character = trailingPos;
        d.range.end.line = lineIdx;
        d.range.end.character = line.length();
        d.severity = lsp::DiagnosticSeverity::Error;
        d.source = "angelscript";
        d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Invalid #include directive: unexpected characters after path", locale);
        diagsOut.push_back(d);
    }

    std::string relPath = line.substr(openQuote + 1, closeQuote - openQuote - 1);
    if (relPath.empty())
    {
        lsp::Diagnostic d;
        d.range.start.line = lineIdx;
        d.range.start.character = openQuote;
        d.range.end.line = lineIdx;
        d.range.end.character = closeQuote + 1;
        d.severity = lsp::DiagnosticSeverity::Error;
        d.source = "angelscript";
        d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Invalid #include directive: empty file path", locale);
        diagsOut.push_back(d);
        return false;
    }

    std::string targetUri = analysis::SymbolCollector::ResolveIncludeUri(baseUri, relPath);
    if (targetUri.empty())
    {
        lsp::Diagnostic d;
        d.range.start.line = lineIdx;
        d.range.start.character = openQuote;
        d.range.end.line = lineIdx;
        d.range.end.character = closeQuote + 1;
        d.severity = lsp::DiagnosticSeverity::Error;
        d.source = "angelscript";
        d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Included file not found: '" + relPath + "'", locale);
        diagsOut.push_back(d);
        return false;
    }

    const Document *openDoc = docResolver ? docResolver(targetUri) : nullptr;
    if (!openDoc)
    {
        std::string filePath = UrlDecode(targetUri);
        if (filePath.starts_with("file:///"))
        {
            filePath = filePath.substr(8);
        }
        else if (filePath.starts_with("file://"))
        {
            filePath = filePath.substr(7);
        }
        std::replace(filePath.begin(), filePath.end(), '/', '\\');

        std::ifstream infile(filePath);
        if (!infile.is_open())
        {
            lsp::Diagnostic d;
            d.range.start.line = lineIdx;
            d.range.start.character = openQuote;
            d.range.end.line = lineIdx;
            d.range.end.character = closeQuote + 1;
            d.severity = lsp::DiagnosticSeverity::Error;
            d.source = "angelscript";
            d.message = angel_lsp::i18n::DiagnosticI18n::Translate("Included file not found: '" + relPath + "'", locale);
            diagsOut.push_back(d);
            return false;
        }
    }

    return true;
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

    void ValidationOracle::SetDefinedWords(const std::vector<std::string> &defines)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_definedWords.clear();
        for (const auto &d : defines)
        {
            if (!d.empty())
            {
                m_definedWords.insert(d);
            }
        }
        SymbolCollector::SetDefinedWords(defines);
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const std::string &code,
                                                               const std::string &currentUri,
                                                               std::function<const Document *(const std::string &)> docResolver)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diagnosticsByUri.clear();

        std::string normCurrentUri = NormalizeUri(currentUri);
        m_activeValidationUri = normCurrentUri;

        if (!m_engine)
        {
            angel_lsp::LspLogger::Error("[Validation] ValidationOracle engine pointer is null!");
            return {};
        }

        angel_lsp::LspLogger::Info("[Validation] Validating URI: '" + currentUri + "' (normalized: '" + normCurrentUri + "')");

        int callbackRes = m_engine->SetMessageCallback(asFUNCTION(MessageCallback), this, asCALL_CDECL_OBJLAST);
        if (callbackRes < 0)
        {
            angel_lsp::LspLogger::Error("[Validation] SetMessageCallback failed with error code: " + std::to_string(callbackRes));
        }

        const char *moduleName = "ValidationModule";
        m_engine->DiscardModule(moduleName);

        asIScriptModule *mod = m_engine->GetModule(moduleName, asGM_ALWAYS_CREATE);

        if (mod)
        {
            std::string *abstractCode = static_cast<std::string *>(m_engine->GetUserData(2000));
            if (abstractCode && !abstractCode->empty())
            {
                angel_lsp::LspLogger::Info("[Validation] Adding Abstract classes script section (length: " + std::to_string(abstractCode->size()) + " bytes)");
                int secRes = mod->AddScriptSection("Abstracts", abstractCode->c_str(), abstractCode->size());
                if (secRes < 0)
                {
                    angel_lsp::LspLogger::Error("[Validation] AddScriptSection('Abstracts') failed with error code: " + std::to_string(secRes));
                }
            }

            std::unordered_set<std::string> visited;
            std::function<void(const std::string &, const std::string &)> loadIncludes =
                [&](const std::string &baseUri, const std::string &srcCode)
                {
                    std::stringstream ss(srcCode);
                    std::string line;
                    size_t lineIdx = 0;
                    while (std::getline(ss, line))
                    {
                        size_t firstNonSpace = line.find_first_not_of(" \t\r");
                        if (firstNonSpace != std::string::npos && line.substr(firstNonSpace).starts_with("#include"))
                        {
                            try
                            {
                                std::vector<lsp::Diagnostic> incDiags;
                                bool validDirect = ValidateIncludeDirective(line, lineIdx, firstNonSpace, baseUri, docResolver, m_locale, incDiags);
                                std::string normBaseUri = NormalizeUri(baseUri);
                                for (const auto &d : incDiags)
                                {
                                    m_diagnosticsByUri[normBaseUri].push_back(d);
                                }

                                if (validDirect)
                                {
                                    std::string relPath = SymbolCollector::ExtractIncludePath(line.substr(firstNonSpace));
                                    std::string targetUri = SymbolCollector::ResolveIncludeUri(baseUri, relPath);
                                    std::string normTargetUri = NormalizeUri(targetUri);
                                    angel_lsp::LspLogger::Info("[Validation] Resolved #include directive: '" + relPath + "' -> '" + normTargetUri + "'");

                                    if (!normTargetUri.empty() && visited.insert(normTargetUri).second)
                                    {
                                        std::string incContent;
                                        const Document *openDoc = docResolver ? docResolver(normTargetUri) : nullptr;
                                        if (openDoc)
                                        {
                                            incContent = openDoc->GetText();
                                            angel_lsp::LspLogger::Info("[Validation] Loaded include content from open document buffer for: '" + normTargetUri + "'");
                                        }
                                        else
                                        {
                                            std::string filePath = UrlDecode(normTargetUri);
                                            if (filePath.starts_with("file:///"))
                                            {
                                                filePath = filePath.substr(8);
                                            }
                                            else if (filePath.starts_with("file://"))
                                            {
                                                filePath = filePath.substr(7);
                                            }
                                            std::replace(filePath.begin(), filePath.end(), '/', '\\');

                                            std::ifstream infile(filePath);
                                            if (infile.is_open())
                                            {
                                                std::stringstream buf;
                                                buf << infile.rdbuf();
                                                incContent = buf.str();
                                                angel_lsp::LspLogger::Info("[Validation] Loaded include content from disk path '" + filePath + "' for: '" + normTargetUri + "'");
                                            }
                                            else
                                            {
                                                angel_lsp::LspLogger::Warn("[Validation] Could not open include file on disk: '" + filePath + "'");
                                            }
                                        }

                                        if (!incContent.empty())
                                        {
                                            loadIncludes(normTargetUri, incContent);
                                            std::string sanitizedInc = SanitizeCodeForEngine(incContent, m_definedWords);
                                            angel_lsp::LspLogger::Info("[Validation] Adding included script section to module: '" + normTargetUri + "' (" + std::to_string(sanitizedInc.size()) + " bytes)");
                                            int secRes = mod->AddScriptSection(normTargetUri.c_str(), sanitizedInc.c_str(), sanitizedInc.size());
                                            if (secRes < 0)
                                            {
                                                angel_lsp::LspLogger::Error("[Validation] AddScriptSection('" + normTargetUri + "') failed with error code: " + std::to_string(secRes));
                                            }
                                        }
                                    }
                                }
                            }
                            catch (const std::exception &ex)
                            {
                                angel_lsp::LspLogger::Error("[Validation] Exception during include processing: " + std::string(ex.what()));
                            }
                        }
                        lineIdx++;
                    }
                };

            if (!normCurrentUri.empty())
            {
                visited.insert(normCurrentUri);
                loadIncludes(normCurrentUri, code);
            }

            std::string sanitizedMain = SanitizeCodeForEngine(code, m_definedWords);
            angel_lsp::LspLogger::Info("[Validation] Adding main script section: '" + normCurrentUri + "' (" + std::to_string(sanitizedMain.size()) + " bytes)");
            int secRes = mod->AddScriptSection(normCurrentUri.c_str(), sanitizedMain.c_str(), sanitizedMain.size());
            if (secRes < 0)
            {
                angel_lsp::LspLogger::Error("[Validation] AddScriptSection('" + normCurrentUri + "') failed with error code: " + std::to_string(secRes));
            }

            bool hasAbstracts = (abstractCode && !abstractCode->empty());

            angel_lsp::LspLogger::Info("[Validation] Executing mod->Build() for ValidationModule...");
            int r = mod->Build();
            std::string statusStr = (r >= 0) ? "SUCCESS" : ("BUILD_ERROR (code " + std::to_string(r) + ")");
            angel_lsp::LspLogger::Info("[Validation] mod->Build() completed -> " + statusStr);

            if (r < 0 && hasAbstracts)
            {
                angel_lsp::LspLogger::Warn("[Validation] mod->Build() failed with Abstracts section present. Retrying build without Abstracts to isolate user code diagnostics...");
                m_diagnosticsByUri.clear();
                m_engine->DiscardModule(moduleName);
                mod = m_engine->GetModule(moduleName, asGM_ALWAYS_CREATE);
                if (mod)
                {
                    if (!normCurrentUri.empty())
                    {
                        visited.clear();
                        visited.insert(normCurrentUri);
                        loadIncludes(normCurrentUri, code);
                    }
                    mod->AddScriptSection(normCurrentUri.c_str(), sanitizedMain.c_str(), sanitizedMain.size());
                    r = mod->Build();
                    std::string retryStatusStr = (r >= 0) ? "SUCCESS" : ("BUILD_ERROR (code " + std::to_string(r) + ")");
                    angel_lsp::LspLogger::Info("[Validation] Retry mod->Build() (without Abstracts) completed -> " + retryStatusStr);
                }
            }
        }
        else
        {
            angel_lsp::LspLogger::Error("[Validation] Failed to create module 'ValidationModule'!");
        }

        m_engine->ClearMessageCallback();
        m_engine->DiscardModule(moduleName);

        auto it = m_diagnosticsByUri.find(normCurrentUri);
        size_t diagCount = (it != m_diagnosticsByUri.end()) ? it->second.size() : 0;
        angel_lsp::LspLogger::Info("[Validation] Returning " + std::to_string(diagCount) + " diagnostic(s) for URI: '" + normCurrentUri + "'");

        if (it != m_diagnosticsByUri.end())
        {
            return it->second;
        }
        return {};
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
            std::string logMsg = "[Validation] [Abstracts Section Warning] (" + std::to_string(msg->row) + ":" + std::to_string(msg->col) + "): " + msg->message;
            angel_lsp::LspLogger::Warn(logMsg);
            return;
        }

        std::string sectionStr = msg->section ? msg->section : "";
        std::string targetUri = sectionStr;
        if (targetUri.empty() || targetUri == "LSP_Doc")
        {
            targetUri = m_activeValidationUri;
        }

        std::string normTargetUri = NormalizeUri(targetUri);
        if (normTargetUri.empty())
        {
            normTargetUri = m_activeValidationUri;
        }

        std::string logMsg = "[Validation] [MessageCallback] " + std::string(msg->type == asMSGTYPE_ERROR ? "ERROR " : (msg->type == asMSGTYPE_WARNING ? "WARN " : "INFO "))
            + "(" + normTargetUri + ":" + std::to_string(msg->row) + ":" + std::to_string(msg->col) + "): " + msg->message;

        if (msg->type == asMSGTYPE_ERROR)
        {
            angel_lsp::LspLogger::Error(logMsg);
        }
        else if (msg->type == asMSGTYPE_WARNING)
        {
            angel_lsp::LspLogger::Warn(logMsg);
        }
        else
        {
            angel_lsp::LspLogger::Info(logMsg);
        }

        lsp::Diagnostic d;

        // AngelScript uses 1-based lines and columns
        d.range.start.line = msg->row > 0 ? msg->row - 1 : 0;
        d.range.start.character = msg->col > 0 ? msg->col - 1 : 0;

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

        m_diagnosticsByUri[normTargetUri].push_back(d);
    }

} // namespace analysis
