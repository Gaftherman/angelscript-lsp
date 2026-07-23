/**
 * @file ValidationOracle.cpp
 * @brief Implementation of ValidationOracle pipeline orchestrator.
 * @ingroup Analysis
 */

#include "ValidationOracle.h"
#include "analysis/validation/rules/SyntaxErrorRule.h"
#include "analysis/validation/rules/TypeValidationRule.h"
#include "analysis/validation/rules/FunctionDeclRule.h"
#include "analysis/validation/rules/ClassDeclRule.h"
#include "analysis/validation/rules/ArgumentRule.h"
#include "analysis/validators/ClassValidator.h"
#include "analysis/validators/ControlFlowValidator.h"
#include "analysis/validators/EnumTypeDefValidator.h"
#include "analysis/validators/ExpressionValidator.h"
#include "analysis/validators/FunctionValidator.h"
#include "analysis/validators/ImportValidator.h"
#include "analysis/validators/UsingValidator.h"
#include <spdlog/fmt/fmt.h>
#include <sstream>

namespace analysis
{
    ValidationOracle::ValidationOracle(i18n::Locale locale)
        : m_locale(locale)
    {
        // Register pipeline rules in strict evaluation order
        m_rules.push_back(std::make_unique<validation::SyntaxErrorRule>());
        m_rules.push_back(std::make_unique<validation::TypeValidationRule>());
        m_rules.push_back(std::make_unique<validation::FunctionDeclRule>());
        m_rules.push_back(std::make_unique<validation::ClassDeclRule>());
        m_rules.push_back(std::make_unique<validation::ArgumentRule>());
    }

    void ValidationOracle::SetDefinedWords(const std::vector<std::string> &defines)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_definedWords.clear();
        for (const auto &def : defines)
        {
            m_definedWords.insert(def);
        }
    }

    static std::string FilterInactivePreprocessorBlocks(std::string_view code, const ankerl::unordered_dense::set<std::string> &definedWords)
    {
        std::string codeStr(code);
        std::istringstream stream(codeStr);
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

    void ValidationOracle::ValidateIncludeDirectives(const Document &doc,
                                                     const std::function<const Document *(const std::string &)> &docResolver,
                                                     std::vector<lsp::Diagnostic> &diags)
    {
        std::istringstream stream(doc.GetText());
        std::string line;
        uint32_t lineIdx = 0;

        const auto &strs = i18n::GetStrings(m_locale);

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
                    d.message = std::string(strs.diagMissingIncludeDelimiter);
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
                        d.message = std::string(strs.diagUnclosedIncludeDelimiter);
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
                            d.message = std::string(strs.diagUnexpectedCharsAfterInclude);
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
                                d.message = fmt::format(fmt::runtime(strs.diagIncludedFileNotFound), incPath);
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }
            lineIdx++;
        }
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const Document &doc,
                                                                 const SymbolTable &localTable,
                                                                 const SymbolTable &globalTable,
                                                                 std::function<const Document *(const std::string &)> docResolver)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<lsp::Diagnostic> diags;

        // 1. Preprocessor include directives check
        ValidateIncludeDirectives(doc, docResolver, diags);

        // 2. Filter inactive preprocessor blocks
        std::string filteredCode = FilterInactivePreprocessorBlocks(doc.GetText(), m_definedWords);
        Document activeDoc(doc.GetUri(), filteredCode);

        SymbolResolver resolver;
        validation::ValidationContext ctx{
            .document = activeDoc,
            .symbolTable = localTable,
            .symbolResolver = resolver,
            .rootNode = activeDoc.RootNode(),
            .globalTable = &globalTable,
            .locale = m_locale,
            .docResolver = docResolver
        };

        // 3. Execute pipeline rules sequentially
        for (const auto &rule : m_rules)
        {
            auto ruleDiags = rule->run(ctx);
            diags.insert(diags.end(), ruleDiags.begin(), ruleDiags.end());
        }

        // 4. Specialized AST node validators dispatch walk
        TSNode root = activeDoc.RootNode();
        if (!ts_node_is_null(root))
        {
            auto walkNode = [&](auto self, TSNode node) -> void
            {
                const char *type = ts_node_type(node);
                if (type)
                {
                    std::string tStr(type);
                    if (tStr == "using_statement" || tStr == "using_directive" || tStr == "using")
                    {
                        auto uDiags = validators::UsingValidator::Validate(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), uDiags.begin(), uDiags.end());
                    }
                    else if (tStr == "import_statement" || tStr == "import_declaration" || tStr == "import")
                    {
                        auto iDiags = validators::ImportValidator::Validate(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), iDiags.begin(), iDiags.end());
                    }
                    else if (tStr == "enum_declaration" || tStr == "enum")
                    {
                        auto eDiags = validators::EnumTypeDefValidator::ValidateEnum(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), eDiags.begin(), eDiags.end());
                    }
                    else if (tStr == "typedef_declaration" || tStr == "typedef")
                    {
                        auto tDiags = validators::EnumTypeDefValidator::ValidateTypedef(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), tDiags.begin(), tDiags.end());
                    }
                    else if (tStr == "func_declaration" || tStr == "function_declaration")
                    {
                        auto fDiags = validators::FunctionValidator::ValidateFunction(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), fDiags.begin(), fDiags.end());

                        auto cfDiags = validators::ControlFlowValidator::ValidateControlFlow(node, activeDoc, m_locale);
                        diags.insert(diags.end(), cfDiags.begin(), cfDiags.end());

                        auto opDiags = validators::ClassValidator::ValidateOperatorOverloads(node, activeDoc, m_locale);
                        diags.insert(diags.end(), opDiags.begin(), opDiags.end());
                    }
                    else if (tStr == "class_declaration" || tStr == "class")
                    {
                        auto cDiags = validators::ClassValidator::ValidateClass(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), cDiags.begin(), cDiags.end());
                    }
                    else if (tStr == "interface_declaration" || tStr == "interface")
                    {
                        auto iDiags = validators::ClassValidator::ValidateInterface(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), iDiags.begin(), iDiags.end());
                    }
                    else if (tStr == "virtual_property" || tStr == "virtprop")
                    {
                        auto vpDiags = validators::ClassValidator::ValidateVirtProp(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), vpDiags.begin(), vpDiags.end());
                    }
                    else if (tStr == "binary_expression" || tStr == "math_expression" || tStr == "logic_expression")
                    {
                        auto exDiags = validators::ExpressionValidator::ValidateExpression(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), exDiags.begin(), exDiags.end());
                    }
                    else if (tStr == "cast" || tStr == "cast_expression")
                    {
                        auto cstDiags = validators::ExpressionValidator::ValidateCast(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), cstDiags.begin(), cstDiags.end());
                    }
                    else if (tStr == "assignment_expression" || tStr == "assign")
                    {
                        auto asgDiags = validators::ExpressionValidator::ValidateAssign(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), asgDiags.begin(), asgDiags.end());

                        auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
                    }
                    else if (tStr == "switch_statement" || tStr == "switch")
                    {
                        auto swDiags = validators::ControlFlowValidator::ValidateSwitch(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), swDiags.begin(), swDiags.end());
                    }
                    else if (tStr == "foreach_statement" || tStr == "foreach")
                    {
                        auto feDiags = validators::ControlFlowValidator::ValidateForeach(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), feDiags.begin(), feDiags.end());
                    }
                    else if (tStr == "member_expression" || tStr == "field_expression" || tStr == "member_access")
                    {
                        auto mDiags = validators::ExpressionValidator::ValidateMemberAccess(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), mDiags.begin(), mDiags.end());
                    }
                    else if (tStr == "ternary_expression" || tStr == "conditional_expression" || tStr == "ternary")
                    {
                        auto terDiags = validators::ExpressionValidator::ValidateTernary(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), terDiags.begin(), terDiags.end());
                    }
                    else if (tStr == "call_expression" || tStr == "function_call" || tStr == "call")
                    {
                        auto argDiags = validators::ExpressionValidator::ValidateCallArguments(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), argDiags.begin(), argDiags.end());

                        auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
                    }
                    else if (tStr == "update_expression" || tStr == "postfix_inc_dec_expression" || tStr == "prefix_inc_dec_expression" || tStr == "inc_dec_expression")
                    {
                        auto incDiags = validators::ExpressionValidator::ValidateIncrementDecrement(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), incDiags.begin(), incDiags.end());
                    }
                    else if (tStr == "expression_statement" || tStr == "expression")
                    {
                        auto argDiags = validators::ExpressionValidator::ValidateCallArguments(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), argDiags.begin(), argDiags.end());

                        auto incDiags = validators::ExpressionValidator::ValidateIncrementDecrement(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), incDiags.begin(), incDiags.end());
                    }
                    else if (tStr == "variable_declaration" || tStr == "construct_call" || tStr == "construct")
                    {
                        auto mixinDiags = validators::ClassValidator::ValidateInstantiation(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), mixinDiags.begin(), mixinDiags.end());

                        auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, activeDoc, globalTable, localTable, m_locale);
                        diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
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

        return diags;
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(const Document &doc,
                                                               const SymbolTable &symbolTable,
                                                               const SymbolResolver &symbolResolver)
    {
        SymbolTable emptyGlobals;
        return ValidateSync(doc, symbolTable, emptyGlobals, nullptr);
    }

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(std::string_view code,
                                                               std::string_view currentUri,
                                                               std::function<const Document *(const std::string &)> docResolver,
                                                               const SymbolTable *globalTable)
    {
        std::string uriStr(currentUri);
        if (uriStr.empty())
        {
            uriStr = "file:///temp_validation.as";
        }
        Document doc(uriStr, std::string(code));
        SymbolTable localTable;
        SymbolTable dummyGlobal;
        const SymbolTable &gTable = globalTable ? *globalTable : dummyGlobal;

        return ValidateSync(doc, localTable, gTable, docResolver);
    }
}
