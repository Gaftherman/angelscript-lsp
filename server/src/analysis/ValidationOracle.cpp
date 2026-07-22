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

/**
 * @brief Post-processes sanitized code to complete incomplete statements.
 *
 * Injects a synthetic `;` at the end of lines that end with a bare identifier
 * (no semicolon or block delimiter) while inside a brace scope. This forces
 * AngelScript to evaluate `hola` as a complete expression-statement and emit
 * the correct "undefined identifier" error instead of silently absorbing it.
 * Also closes any unclosed brace blocks to handle truncated (mid-typing) files.
 *
 * @param code The sanitized code produced by the preprocessor pass.
 * @return Code with incomplete statements terminated and open braces closed.
 */
static std::string CompleteIncompleteStatements(const std::string &code)
{
    std::string result;
    result.reserve(code.size() + 64);

    std::stringstream ss(code);
    std::string line;
    int braceDepth = 0;
    bool firstLine = true;

    while (std::getline(ss, line))
    {
        if (!firstLine)
            result += "\n";
        firstLine = false;

        // Trim trailing whitespace / \r for analysis only
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r')
            trimmed.pop_back();
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
            trimmed.pop_back();

        // Strip trailing line-comment so we examine the actual code last token
        // (Simple heuristic: find first // that is not inside a string)
        {
            bool inStr = false;
            char strChar = 0;
            for (size_t i = 0; i < trimmed.size(); ++i)
            {
                if (!inStr && (trimmed[i] == '"' || trimmed[i] == '\'' ))
                {
                    inStr = true;
                    strChar = trimmed[i];
                }
                else if (inStr && trimmed[i] == strChar && (i == 0 || trimmed[i - 1] != '\\'))
                {
                    inStr = false;
                }
                else if (!inStr && i + 1 < trimmed.size() && trimmed[i] == '/' && trimmed[i + 1] == '/')
                {
                    trimmed = trimmed.substr(0, i);
                    // Re-trim trailing whitespace
                    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
                        trimmed.pop_back();
                    break;
                }
            }
        }

        // Depth BEFORE this line (used to detect "inside a block")
        int depthBefore = braceDepth;

        // Count brace changes on this line (simple, not string/comment aware for depth tracking)
        for (char c : trimmed)
        {
            if (c == '{')
                braceDepth++;
            else if (c == '}')
                braceDepth--;
        }

        // Decide whether to inject ';'
        // Only inject when:
        //   1. We are inside a brace block (depthBefore > 0).
        //   2. The line is not a comment, preprocessor directive, or brace-only line.
        //   3. The last character is an identifier character (a-z, A-Z, 0-9, _).
        //   4. The FIRST word on the line is NOT a declaration keyword that could
        //      start a valid multi-line construct (e.g. namespace, class, void, float…).
        //      This prevents injecting ';' into things like "namespace Math" that live
        //      inside an outer namespace block at depth >= 1.
        static const std::unordered_set<std::string> s_declarationKeywords = {
            "namespace", "class", "interface", "mixin", "enum", "struct",
            "funcdef", "typedef", "import", "from", "using",
            "void", "bool", "int", "int8", "int16", "int32", "int64",
            "uint", "uint8", "uint16", "uint32", "uint64",
            "float", "double", "auto", "string", "array", "dictionary",
            "private", "protected", "public", "shared", "abstract",
            "final", "override", "explicit", "external", "export", "const",
            "return", "if", "else", "for", "foreach", "while", "do",
            "switch", "case", "default", "break", "continue",
            "try", "catch", "new", "delete", "cast", "in", "out", "inout",
            "get", "set", "is", "not", "and", "or", "xor",
            "true", "false", "null", "this", "super", "self"
        };

        bool shouldInject = false;
        if (depthBefore > 0 && !trimmed.empty())
        {
            size_t firstNS = trimmed.find_first_not_of(" \t");
            if (firstNS != std::string::npos)
            {
                char firstChar = trimmed[firstNS];
                // Skip comment lines, preprocessor lines, and block delimiters
                bool isSkipped = (firstChar == '/' || firstChar == '#' ||
                                  firstChar == '{' || firstChar == '}' ||
                                  firstChar == '~');  // destructor: ~ClassName()
                if (!isSkipped)
                {
                    char lastChar = trimmed.back();
                    // Only inject if last char is an identifier character
                    if (std::isalpha(static_cast<unsigned char>(lastChar)) ||
                        std::isdigit(static_cast<unsigned char>(lastChar)) ||
                        lastChar == '_')
                    {
                        // Extract the first word to check for declaration keywords
                        size_t wordEnd = firstNS;
                        while (wordEnd < trimmed.size() &&
                               (std::isalnum(static_cast<unsigned char>(trimmed[wordEnd])) ||
                                trimmed[wordEnd] == '_'))
                        {
                            wordEnd++;
                        }
                        std::string firstWord = trimmed.substr(firstNS, wordEnd - firstNS);

                        if (s_declarationKeywords.find(firstWord) == s_declarationKeywords.end())
                        {
                            shouldInject = true;
                        }
                    }
                }
            }
        }

        if (shouldInject)
        {
            result += trimmed + ";";
        }
        else
        {
            result += line;
        }
    }

    // Close any unclosed brace blocks (handles truncated / mid-typing files).
    // Each synthetic closing is preceded by ';' to force evaluation of any
    // dangling statement before the closing brace.
    while (braceDepth > 0)
    {
        result += "\n;}";
        braceDepth--;
    }

    return result;
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
    return CompleteIncompleteStatements(sanitizedCode);
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
        m_activeValidationUri = currentUri;

        if (!m_engine)
        {
            return {};
        }

        angel_lsp::LspLogger::Info("[Validation] Validating URI: '" + currentUri + "'");

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
                                    for (const auto &d : incDiags)
                                    {
                                        m_diagnosticsByUri[baseUri].push_back(d);
                                    }

                                    if (validDirect)
                                    {
                                        std::string relPath = SymbolCollector::ExtractIncludePath(line.substr(firstNonSpace));
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
                                                std::string sanitizedInc = SanitizeCodeForEngine(incContent, m_definedWords);
                                                mod->AddScriptSection(targetUri.c_str(), sanitizedInc.c_str(), sanitizedInc.size());
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

                loadIncludes(currentUri, code);
            }

            std::string sanitizedMain = SanitizeCodeForEngine(code, m_definedWords);
            mod->AddScriptSection(currentUri.c_str(), sanitizedMain.c_str(), sanitizedMain.size());

            int r = mod->Build();
            angel_lsp::LspLogger::Info("[Validation] mod->Build() returned " + std::to_string(r));
        }

        m_engine->ClearMessageCallback();
        m_engine->DiscardModule(moduleName);

        return m_diagnosticsByUri[currentUri];
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

        std::string sectionStr = msg->section ? msg->section : "";
        std::string targetUri = sectionStr;
        if (targetUri.empty() || targetUri == "LSP_Doc")
        {
            targetUri = m_activeValidationUri;
        }

        std::string logMsg = "[Validation] " + std::string(msg->type == asMSGTYPE_ERROR ? "ERROR " : (msg->type == asMSGTYPE_WARNING ? "WARN " : "INFO "))
            + "(" + targetUri + ":" + std::to_string(msg->row) + ":" + std::to_string(msg->col) + "): " + msg->message;

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

        m_diagnosticsByUri[targetUri].push_back(d);
    }

} // namespace analysis
