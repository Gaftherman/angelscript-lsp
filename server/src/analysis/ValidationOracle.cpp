/**
 * @file ValidationOracle.cpp
 * @brief Diagnostic extraction and validation engine using pure Tree-Sitter parsing.
 * @ingroup Analysis
 */

#include "ValidationOracle.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"
#include "document/Document.h"
#include "i18n/LspStrings.h"
#include <unordered_set>
#include <sstream>

namespace analysis
{

    static std::string FilterInactivePreprocessorBlocks(const std::string &code, const std::unordered_set<std::string> &definedWords)
    {
        std::istringstream stream(code);
        std::string line;
        std::string result;
        result.reserve(code.size());

        std::vector<bool> activeStack = {true};

        while (std::getline(stream, line))
        {
            size_t start = line.find_first_not_of(" \t");
            if (start != std::string::npos && line[start] == '#')
            {
                size_t cmdStart = line.find_first_not_of(" \t", start + 1);
                if (cmdStart == std::string::npos)
                {
                    result += "\n";
                    continue;
                }

                std::string directive = line.substr(cmdStart);
                size_t wordEnd = directive.find_first_of(" \t\r\n");
                std::string cmd = (wordEnd == std::string::npos) ? directive : directive.substr(0, wordEnd);
                std::string rest = (wordEnd == std::string::npos) ? "" : directive.substr(wordEnd + 1);

                if (cmd == "if" || cmd == "ifdef" || cmd == "ifndef")
                {
                    size_t argStart = rest.find_first_not_of(" \t");
                    std::string arg = (argStart == std::string::npos) ? "" : rest.substr(argStart);
                    size_t argEnd = arg.find_first_of(" \t\r\n");
                    if (argEnd != std::string::npos)
                    {
                        arg = arg.substr(0, argEnd);
                    }

                    bool isNegated = (!arg.empty() && arg[0] == '!');
                    std::string word = isNegated ? arg.substr(1) : arg;

                    bool wordDefined = definedWords.contains(word);
                    bool condition = (cmd == "ifndef") ? !wordDefined : (isNegated ? !wordDefined : wordDefined);

                    bool parentActive = activeStack.back();
                    activeStack.push_back(parentActive && condition);

                    result += "\n";
                    continue;
                }
                else if (cmd == "else")
                {
                    if (activeStack.size() > 1)
                    {
                        bool current = activeStack.back();
                        activeStack.pop_back();
                        bool parentActive = activeStack.back();
                        activeStack.push_back(parentActive && !current);
                    }
                    result += "\n";
                    continue;
                }
                else if (cmd == "endif")
                {
                    if (activeStack.size() > 1)
                    {
                        activeStack.pop_back();
                    }
                    result += "\n";
                    continue;
                }
            }

            bool currentlyActive = activeStack.back();
            if (!currentlyActive)
            {
                result += "\n";
            }
            else
            {
                result += line;
                result += "\n";
            }
        }

        return result;
    }

    ValidationOracle::ValidationOracle(i18n::Locale locale)
        : m_locale(locale)
    {
    }

    ValidationOracle::~ValidationOracle()
    {
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const Document &doc,
                                                                 const SymbolTable &localTable,
                                                                 const SymbolTable &globalTable,
                                                                 std::function<const Document *(const std::string &)> docResolver)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<lsp::Diagnostic> diags;

        ValidateIncludeDirective(doc, docResolver, diags);

        std::string filteredCode = FilterInactivePreprocessorBlocks(doc.GetText(), m_definedWords);
        Document activeDoc(doc.GetUri(), filteredCode);

        CollectSyntaxErrors(activeDoc, diags);
        CollectSemanticErrors(activeDoc, localTable, globalTable, diags);

        return diags;
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const std::string &code,
                                                                 const std::string &currentUri,
                                                                 std::function<const Document *(const std::string &)> docResolver,
                                                                 const SymbolTable *globalTable)
    {
        Document doc(currentUri, code);
        SymbolTable localTable;
        if (globalTable)
        {
            localTable.MergeGlobals(*globalTable);
        }
        SymbolCollector::CollectGlobals(doc, localTable, docResolver);
        SymbolCollector::CollectLocals(doc, localTable);

        SymbolTable emptyGlobal;
        const SymbolTable &gTable = globalTable ? *globalTable : emptyGlobal;

        return ValidateSync(doc, localTable, gTable, docResolver);
    }

    void ValidationOracle::SetDefinedWords(const std::vector<std::string> &defines)
    {
        m_definedWords.clear();
        for (const auto &w : defines)
        {
            m_definedWords.insert(w);
        }
        SymbolCollector::SetDefinedWords(defines);
    }

    void ValidationOracle::CollectSyntaxErrors(const Document &doc, std::vector<lsp::Diagnostic> &diags)
    {
        TSNode root = doc.RootNode();
        if (ts_node_is_null(root))
        {
            return;
        }

        auto walkNode = [&](auto self, TSNode node) -> void
        {
            if (ts_node_is_error(node) || ts_node_is_missing(node))
            {
                TSPoint start = ts_node_start_point(node);
                TSPoint end = ts_node_end_point(node);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = (end.row == start.row && end.column == start.column) ? start.column + 1 : end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "tree-sitter";

                std::string_view sv = doc.SourceAt(node);
                std::string nodeText(sv.begin(), sv.end());
                if (ts_node_is_missing(node))
                {
                    d.message = (m_locale == i18n::Locale::ES) ? "Error de sintaxis: se esperaba un token" : "Syntax error: missing expected token";
                }
                else if (!nodeText.empty())
                {
                    d.message = (m_locale == i18n::Locale::ES) ? ("Error de sintaxis: token inesperado '" + nodeText + "'") : ("Syntax error: unexpected token '" + nodeText + "'");
                }
                else
                {
                    d.message = (m_locale == i18n::Locale::ES) ? "Error de sintaxis: token inesperado" : "Syntax error: unexpected token";
                }

                diags.push_back(d);
                return;
            }

            uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(node, i);
                self(self, child);
            }
        };

        walkNode(walkNode, root);
    }

    void ValidationOracle::CollectSemanticErrors(const Document &doc, const SymbolTable &localTable, const SymbolTable &globalTable, std::vector<lsp::Diagnostic> &diags)
    {
        TSNode root = doc.RootNode();
        if (ts_node_is_null(root))
        {
            return;
        }

        static const std::unordered_set<std::string> builtins = {
            "int", "uint", "int8", "int16", "int64", "uint8", "uint16", "uint64",
            "float", "double", "bool", "void", "string", "array", "auto", "const",
            "override", "final", "abstract", "shared", "mixin", "namespace", "class",
            "interface", "enum", "struct", "typedef", "funcdef", "import", "from",
            "true", "false", "null", "this", "super"};

        auto isResolved = [&](const std::string &name, uint32_t line, uint32_t col) -> bool
        {
            if (builtins.contains(name) || m_definedWords.contains(name))
            {
                return true;
            }
            if (localTable.FindLocalByNameAt(name, line, col) || localTable.FindLocalByName(name) || localTable.FindGlobalByName(name) || localTable.FindFirst(name) || localTable.FindByNameDeep(name))
            {
                return true;
            }
            if (globalTable.FindGlobalByName(name) || globalTable.FindFirst(name) || globalTable.FindByNameDeep(name))
            {
                return true;
            }
            if (SymbolResolver::ResolveAt(doc, localTable, line, col) != nullptr)
            {
                return true;
            }
            if (SymbolResolver::ResolveAt(doc, globalTable, line, col) != nullptr)
            {
                return true;
            }
            return false;
        };

        auto walkNode = [&](auto self, TSNode node) -> void
        {
            const char *type = ts_node_type(node);
            if (type && std::string(type) == "identifier")
            {
                TSNode parent = ts_node_parent(node);
                const char *parentType = parent.id ? ts_node_type(parent) : nullptr;

                bool shouldCheck = true;
                if (parentType)
                {
                    std::string pStr(parentType);
                    if (pStr == "class_declaration" || pStr == "function_declaration" ||
                        pStr == "variable_declarator" || pStr == "parameter" ||
                        pStr == "enum_declaration" || pStr == "interface_declaration" ||
                        pStr == "namespace_declaration" || pStr == "field_declaration" ||
                        pStr == "enum_member")
                    {
                        shouldCheck = false;
                    }
                    else if (pStr == "member_expression" || pStr == "field_access")
                    {
                        TSNode memberNode = ts_node_child_by_field_name(parent, "member", sizeof("member") - 1);
                        if (ts_node_is_null(memberNode))
                        {
                            memberNode = ts_node_child_by_field_name(parent, "field", sizeof("field") - 1);
                        }
                        if (!ts_node_is_null(memberNode) && ts_node_eq(memberNode, node))
                        {
                            shouldCheck = false;
                        }
                    }
                }

                if (shouldCheck)
                {
                    std::string_view sv = doc.SourceAt(node);
                    std::string name(sv.begin(), sv.end());
                    TSPoint start = ts_node_start_point(node);

                    if (!name.empty() && !isResolved(name, start.row, start.column))
                    {
                        TSPoint end = ts_node_end_point(node);

                        lsp::Diagnostic d;
                        d.range.start.line = start.row;
                        d.range.start.character = start.column;
                        d.range.end.line = end.row;
                        d.range.end.character = end.column;
                        d.severity = lsp::DiagnosticSeverity::Error;
                        d.source = "angelscript";
                        d.message = (m_locale == i18n::Locale::ES) ? ("Identificador o símbolo no declarado '" + name + "'") : ("Undeclared identifier or symbol '" + name + "'");

                        diags.push_back(d);
                    }
                }
            }

            uint32_t childCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                TSNode child = ts_node_child(node, i);
                self(self, child);
            }
        };

        walkNode(walkNode, root);
    }

    void ValidationOracle::ValidateIncludeDirective(const Document &doc,
                                                     const std::function<const Document *(const std::string &)> &docResolver,
                                                     std::vector<lsp::Diagnostic> &diags)
    {
        std::istringstream stream(doc.GetText());
        std::string line;
        uint32_t lineIdx = 0;

        auto resolveRelative = [](const std::string &baseUri, const std::string &relPath) -> std::string
        {
            if (relPath.find("://") != std::string::npos || (!relPath.empty() && relPath[0] == '/'))
            {
                return relPath;
            }
            size_t lastSlash = baseUri.find_last_of("/\\");
            if (lastSlash != std::string::npos)
            {
                return baseUri.substr(0, lastSlash + 1) + relPath;
            }
            return relPath;
        };

        while (std::getline(stream, line))
        {
            size_t start = line.find_first_not_of(" \t");
            if (start != std::string::npos && line.compare(start, 8, "#include") == 0)
            {
                size_t pathStart = line.find_first_of("\"<", start + 8);
                if (pathStart == std::string::npos)
                {
                    lsp::Diagnostic d;
                    d.range.start.line = lineIdx;
                    d.range.start.character = (uint32_t)start;
                    d.range.end.line = lineIdx;
                    d.range.end.character = (uint32_t)line.length();
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "preprocessor";
                    d.message = (m_locale == i18n::Locale::ES) ? "Error de sintaxis: falta delimitador de ruta en #include" : "Syntax error: missing include path delimiter";
                    diags.push_back(d);
                }
                else
                {
                    char openDelim = line[pathStart];
                    char closeDelim = (openDelim == '"') ? '"' : '>';
                    size_t pathEnd = line.find(closeDelim, pathStart + 1);

                    if (pathEnd == std::string::npos)
                    {
                        lsp::Diagnostic d;
                        d.range.start.line = lineIdx;
                        d.range.start.character = (uint32_t)pathStart;
                        d.range.end.line = lineIdx;
                        d.range.end.character = (uint32_t)line.length();
                        d.severity = lsp::DiagnosticSeverity::Error;
                        d.source = "preprocessor";
                        d.message = (m_locale == i18n::Locale::ES) ? "Error de sintaxis: delimitador de ruta sin cerrar en #include" : "Syntax error: unclosed path delimiter in #include";
                        diags.push_back(d);
                    }
                    else
                    {
                        size_t trailing = line.find_first_not_of(" \t\r\n", pathEnd + 1);
                        if (trailing != std::string::npos && line.compare(trailing, 2, "//") != 0 && line.compare(trailing, 2, "/*") != 0)
                        {
                            lsp::Diagnostic d;
                            d.range.start.line = lineIdx;
                            d.range.start.character = (uint32_t)trailing;
                            d.range.end.line = lineIdx;
                            d.range.end.character = (uint32_t)line.length();
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "preprocessor";
                            d.message = (m_locale == i18n::Locale::ES) ? "Error de sintaxis: caracteres no esperados después de directiva #include" : "Syntax error: unexpected characters after #include directive";
                            diags.push_back(d);
                        }

                        std::string incPath = line.substr(pathStart + 1, pathEnd - pathStart - 1);
                        if (!incPath.empty())
                        {
                            std::string normInc = incPath;
                            if (normInc.find('.') == std::string::npos)
                            {
                                normInc += ".as";
                            }
                            std::string fullIncUri = resolveRelative(doc.GetUri(), normInc);

                            bool resolved = false;
                            if (docResolver)
                            {
                                resolved = docResolver(incPath) || docResolver(normInc) || docResolver(fullIncUri) || docResolver("file:///" + normInc);
                            }

                            if (!resolved)
                            {
                                lsp::Diagnostic d;
                                d.range.start.line = lineIdx;
                                d.range.start.character = (uint32_t)pathStart;
                                d.range.end.line = lineIdx;
                                d.range.end.character = (uint32_t)pathEnd + 1;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "preprocessor";
                                d.message = (m_locale == i18n::Locale::ES) ? ("Archivo incluido no encontrado: '" + incPath + "'") : ("Included file not found: '" + incPath + "'");
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }
            lineIdx++;
        }
    }

} // namespace analysis
