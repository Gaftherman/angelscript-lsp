/**
 * @file ValidationOracle.cpp
 * @brief Diagnostic extraction and validation engine using pure Tree-Sitter parsing.
 * @ingroup Analysis
 */

#include "ValidationOracle.h"
#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"
#include "analysis/TypeEvaluator.h"
#include "analysis/validators/UsingValidator.h"
#include "analysis/validators/ImportValidator.h"
#include "analysis/validators/EnumTypeDefValidator.h"
#include "analysis/validators/FunctionValidator.h"
#include "analysis/validators/ClassValidator.h"
#include "analysis/validators/ExpressionValidator.h"
#include "analysis/validators/ControlFlowValidator.h"
#include "document/Document.h"
#include "i18n/LspStrings.h"
#include <ankerl/unordered_dense.h>
#include <spdlog/fmt/fmt.h>
#include <sstream>

namespace analysis
{

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

    std::vector<lsp::Diagnostic> ValidationOracle::ValidateSync(std::string_view code,
                                                                 std::string_view currentUri,
                                                                 std::function<const Document *(const std::string &)> docResolver,
                                                                 const SymbolTable *globalTable)
    {
        Document doc{std::string(currentUri), std::string(code)};
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

        const auto &strs = i18n::GetStrings(m_locale);

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
                    d.message = std::string(strs.diagMissingExpectedToken);
                }
                else if (!nodeText.empty())
                {
                    d.message = fmt::format(fmt::runtime(strs.diagUnexpectedToken), nodeText);
                }
                else
                {
                    d.message = std::string(strs.diagSyntaxError);
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

        const auto &strs = i18n::GetStrings(m_locale);

        static const ankerl::unordered_dense::set<std::string> builtins = {
            "int", "uint", "int8", "int16", "int64", "uint8", "uint16", "uint64",
            "float", "double", "bool", "void", "string", "array", "dictionary", "auto", "const",
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

        // --- Check 1: Duplicate Local Variable / Symbol Redefinition in Same Scope ---
        const auto &locals = localTable.GetLocals();
        for (size_t i = 0; i < locals.size(); ++i)
        {
            for (size_t j = i + 1; j < locals.size(); ++j)
            {
                bool isSameBlockScope = (locals[i]->fullRange.start.line == locals[j]->fullRange.start.line &&
                                         locals[i]->fullRange.end.line == locals[j]->fullRange.end.line);

                if (isSameBlockScope &&
                    locals[i]->name == locals[j]->name &&
                    locals[i]->kind != SymbolKind::Parameter &&
                    locals[j]->kind != SymbolKind::Parameter &&
                    locals[i]->parent == locals[j]->parent)
                {
                    lsp::Diagnostic d;
                    d.range = locals[j]->selectionRange;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagRedefinitionOfSymbol), locals[j]->name);
                    diags.push_back(d);
                }
            }
        }

        auto walkNode = [&](auto self, TSNode node) -> void
        {
            const char *type = ts_node_type(node);

            // --- Modular Validator Dispatches ---
            if (type)
            {
                std::string tStr(type);
                if (tStr == "using_statement" || tStr == "using_directive" || tStr == "using")
                {
                    auto uDiags = validators::UsingValidator::Validate(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), uDiags.begin(), uDiags.end());
                }
                else if (tStr == "import_statement" || tStr == "import_declaration" || tStr == "import")
                {
                    auto iDiags = validators::ImportValidator::Validate(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), iDiags.begin(), iDiags.end());
                }
                else if (tStr == "enum_declaration" || tStr == "enum")
                {
                    auto eDiags = validators::EnumTypeDefValidator::ValidateEnum(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), eDiags.begin(), eDiags.end());
                }
                else if (tStr == "typedef_declaration" || tStr == "typedef")
                {
                    auto tDiags = validators::EnumTypeDefValidator::ValidateTypedef(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), tDiags.begin(), tDiags.end());
                }
                else if (tStr == "func_declaration" || tStr == "function_declaration")
                {
                    auto fDiags = validators::FunctionValidator::ValidateFunction(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), fDiags.begin(), fDiags.end());

                    auto cfDiags = validators::ControlFlowValidator::ValidateControlFlow(node, doc, m_locale);
                    diags.insert(diags.end(), cfDiags.begin(), cfDiags.end());

                    auto opDiags = validators::ClassValidator::ValidateOperatorOverloads(node, doc, m_locale);
                    diags.insert(diags.end(), opDiags.begin(), opDiags.end());
                }
                else if (tStr == "funcdef_declaration" || tStr == "funcdef")
                {
                    auto fdDiags = validators::FunctionValidator::ValidateFuncdef(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), fdDiags.begin(), fdDiags.end());
                }
                else if (tStr == "class_declaration" || tStr == "class")
                {
                    auto cDiags = validators::ClassValidator::ValidateClass(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), cDiags.begin(), cDiags.end());
                }
                else if (tStr == "interface_declaration" || tStr == "interface")
                {
                    auto iDiags = validators::ClassValidator::ValidateInterface(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), iDiags.begin(), iDiags.end());
                }
                else if (tStr == "virtual_property" || tStr == "virtprop")
                {
                    auto vpDiags = validators::ClassValidator::ValidateVirtProp(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), vpDiags.begin(), vpDiags.end());
                }
                else if (tStr == "binary_expression" || tStr == "math_expression" || tStr == "logic_expression")
                {
                    auto exDiags = validators::ExpressionValidator::ValidateExpression(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), exDiags.begin(), exDiags.end());
                }
                else if (tStr == "cast" || tStr == "cast_expression")
                {
                    auto cstDiags = validators::ExpressionValidator::ValidateCast(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), cstDiags.begin(), cstDiags.end());
                }
                else if (tStr == "assignment_expression" || tStr == "assign")
                {
                    auto asgDiags = validators::ExpressionValidator::ValidateAssign(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), asgDiags.begin(), asgDiags.end());

                    auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
                }
                else if (tStr == "switch_statement" || tStr == "switch")
                {
                    auto swDiags = validators::ControlFlowValidator::ValidateSwitch(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), swDiags.begin(), swDiags.end());
                }
                else if (tStr == "foreach_statement" || tStr == "foreach")
                {
                    auto feDiags = validators::ControlFlowValidator::ValidateForeach(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), feDiags.begin(), feDiags.end());
                }
                else if (tStr == "member_expression" || tStr == "field_expression" || tStr == "member_access")
                {
                    auto mDiags = validators::ExpressionValidator::ValidateMemberAccess(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), mDiags.begin(), mDiags.end());
                }
                else if (tStr == "ternary_expression" || tStr == "conditional_expression" || tStr == "ternary")
                {
                    auto terDiags = validators::ExpressionValidator::ValidateTernary(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), terDiags.begin(), terDiags.end());
                }
                else if (tStr == "call_expression" || tStr == "function_call" || tStr == "call")
                {
                    auto argDiags = validators::ExpressionValidator::ValidateCallArguments(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), argDiags.begin(), argDiags.end());

                    auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
                }
                else if (tStr == "update_expression" || tStr == "postfix_inc_dec_expression" || tStr == "prefix_inc_dec_expression" || tStr == "inc_dec_expression")
                {
                    auto incDiags = validators::ExpressionValidator::ValidateIncrementDecrement(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), incDiags.begin(), incDiags.end());
                }
                else if (tStr == "expression_statement" || tStr == "expression")
                {
                    auto argDiags = validators::ExpressionValidator::ValidateCallArguments(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), argDiags.begin(), argDiags.end());

                    auto incDiags = validators::ExpressionValidator::ValidateIncrementDecrement(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), incDiags.begin(), incDiags.end());
                }
                else if (tStr == "variable_declaration" || tStr == "construct_call" || tStr == "construct")
                {
                    auto mixinDiags = validators::ClassValidator::ValidateInstantiation(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), mixinDiags.begin(), mixinDiags.end());

                    auto lamDiags = validators::ExpressionValidator::ValidateLambda(node, doc, globalTable, localTable, m_locale);
                    diags.insert(diags.end(), lamDiags.begin(), lamDiags.end());
                }
            }

            // --- Check 2: Uncalled Method / Function Reference as Standalone Expression Statement ---
            if (type && std::string(type) == "expression_statement")
            {
                uint32_t childCount = ts_node_child_count(node);
                if (childCount > 0)
                {
                    TSNode expr = ts_node_child(node, 0);
                    const char *exprType = ts_node_type(expr);
                    if (exprType && (std::string(exprType) == "member_expression" || std::string(exprType) == "field_access"))
                    {
                        TSNode memberNode = ts_node_child_by_field_name(expr, "member", sizeof("member") - 1);
                        if (ts_node_is_null(memberNode))
                        {
                            memberNode = ts_node_child_by_field_name(expr, "field", sizeof("field") - 1);
                        }
                        if (!ts_node_is_null(memberNode))
                        {
                            TSPoint mStart = ts_node_start_point(memberNode);
                            std::string_view mText = doc.SourceAt(memberNode);
                            SymbolTable combined = localTable;
                            combined.MergeGlobals(globalTable);

                            const Symbol *resolved = SymbolResolver::ResolveAt(doc, combined, mStart.row, mStart.column);
                            if (!resolved)
                            {
                                resolved = combined.FindByNameDeep(mText);
                            }
                            if (resolved && (resolved->kind == SymbolKind::Method || resolved->kind == SymbolKind::Function))
                            {
                                TSPoint start = ts_node_start_point(memberNode);
                                TSPoint end = ts_node_end_point(memberNode);

                                lsp::Diagnostic d;
                                d.range.start.line = start.row;
                                d.range.start.character = start.column;
                                d.range.end.line = end.row;
                                d.range.end.character = end.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = fmt::format(fmt::runtime(strs.diagMethodMustBeCalled), resolved->name);
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }

            // --- Check 3: Type Mismatch in Variable Assignments ---
            if (type && std::string(type) == "variable_declaration")
            {
                TSNode declNode = node;
                TSNode typeNode = ts_node_child_by_field_name(declNode, "type", sizeof("type") - 1);
                if (ts_node_is_null(typeNode))
                {
                    uint32_t count = ts_node_child_count(declNode);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_child(declNode, i);
                        std::string_view cType = ts_node_type(child);
                        std::string_view text = doc.SourceAt(child);
                        if ((cType == "type" || cType == "primitive_type" || cType == "identifier") &&
                            text != "private" && text != "protected" && text != "public" && text != "const")
                        {
                            typeNode = child;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(typeNode))
                {
                    std::string_view lhsType = doc.SourceAt(typeNode);
                    if (lhsType != "auto" && !lhsType.empty())
                    {
                        uint32_t count = ts_node_child_count(declNode);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            TSNode child = ts_node_child(declNode, i);
                            const char *cType = ts_node_type(child);
                            if (cType && (std::string(cType) == "variable_declarator" || std::string(cType) == "init_declarator"))
                            {
                                TSNode valueNode = ts_node_child_by_field_name(child, "value", sizeof("value") - 1);
                                if (ts_node_is_null(valueNode))
                                {
                                    uint32_t subCount = ts_node_child_count(child);
                                    bool foundEq = false;
                                    for (uint32_t j = 0; j < subCount; ++j)
                                    {
                                        TSNode subChild = ts_node_child(child, j);
                                        std::string_view subText = doc.SourceAt(subChild);
                                        if (subText == "=")
                                        {
                                            foundEq = true;
                                            continue;
                                        }
                                        if (foundEq)
                                        {
                                            valueNode = subChild;
                                            break;
                                        }
                                    }
                                }

                                if (!ts_node_is_null(valueNode))
                                {
                                    auto rhsTypeOpt = TypeEvaluator::InferType(valueNode, doc, globalTable, localTable);
                                    if (rhsTypeOpt.has_value())
                                    {
                                        std::string rhsType = rhsTypeOpt.value();
                                        SymbolTable combined = localTable;
                                        combined.MergeGlobals(globalTable);
                                        if (!TypeEvaluator::AreTypesCompatible(lhsType, rhsType, &combined))
                                        {
                                            TSPoint start = ts_node_start_point(child);
                                            TSPoint end = ts_node_end_point(child);

                                            lsp::Diagnostic d;
                                            d.range.start.line = start.row;
                                            d.range.start.character = start.column;
                                            d.range.end.line = end.row;
                                            d.range.end.character = end.column;
                                            d.severity = lsp::DiagnosticSeverity::Error;
                                            d.source = "angelscript";
                                            d.message = fmt::format("Type mismatch: Cannot implicitly convert '{}' to '{}'.", rhsType, lhsType);

                                            diags.push_back(d);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

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
                        d.message = fmt::format(fmt::runtime(strs.diagUndeclaredSymbol), name);

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

} // namespace analysis
