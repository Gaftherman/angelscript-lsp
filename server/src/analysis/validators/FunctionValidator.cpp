/**
 * @file FunctionValidator.cpp
 * @brief Implementation of FunctionValidator for functions, funcdefs, and parameter lists.
 * @ingroup Analysis
 */

#include "FunctionValidator.h"
#include <spdlog/fmt/fmt.h>
#include <ankerl/unordered_dense.h>

namespace analysis::validators
{
    static bool IsPrimitiveOrBuiltinType(std::string_view typeStr)
    {
        std::string s(typeStr);
        // Strip const, &, in, out, inout, @
        const std::vector<std::string> removeWords = {"const", "&inout", "&in", "&out", "&", "@"};
        for (const auto &w : removeWords)
        {
            size_t pos = s.find(w);
            while (pos != std::string::npos)
            {
                s.erase(pos, w.length());
                pos = s.find(w);
            }
        }
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start == std::string::npos)
        {
            return true;
        }
        std::string clean = s.substr(start, end - start + 1);

        return clean == "void" || clean == "bool" || clean == "int" || clean == "uint" ||
               clean == "float" || clean == "double" || clean == "string" || clean == "array" ||
               clean == "int8" || clean == "int16" || clean == "int32" || clean == "int64" ||
               clean == "uint8" || clean == "uint16" || clean == "uint32" || clean == "uint64" ||
               clean == "auto";
    }

    static void ValidateParamList(
        TSNode paramListNode,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale,
        std::vector<lsp::Diagnostic> &diags)
    {
        if (ts_node_is_null(paramListNode))
        {
            return;
        }

        const auto &strs = i18n::GetStrings(locale);
        ankerl::unordered_dense::set<std::string> paramNamesSeen;
        bool sawDefault = false;

        uint32_t count = ts_node_child_count(paramListNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(paramListNode, i);
            const char *cType = ts_node_type(child);
            if (!cType || std::string(cType) != "parameter")
            {
                continue;
            }

            // Extract parameter name
            TSNode nameNode = ts_node_child_by_field_name(child, "name", sizeof("name") - 1);
            if (!ts_node_is_null(nameNode))
            {
                std::string_view pName = doc.SourceAt(nameNode);
                std::string pStr(pName);
                if (paramNamesSeen.contains(pStr))
                {
                    TSPoint start = ts_node_start_point(nameNode);
                    TSPoint end = ts_node_end_point(nameNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagDuplicateParamName), pStr);
                    diags.push_back(d);
                }
                else
                {
                    paramNamesSeen.insert(pStr);
                }
            }

            // Extract parameter type node
            TSNode typeNode = ts_node_child_by_field_name(child, "type", sizeof("type") - 1);
            if (ts_node_is_null(typeNode))
            {
                typeNode = ts_node_child(child, 0);
            }

            std::string pTypeStr;
            if (!ts_node_is_null(typeNode))
            {
                pTypeStr = std::string(doc.SourceAt(typeNode));
                if (!IsPrimitiveOrBuiltinType(pTypeStr))
                {
                    std::string cleanType = pTypeStr;
                    const std::vector<std::string> removeWords = {"const", "&inout", "&in", "&out", "&", "@"};
                    for (const auto &w : removeWords)
                    {
                        size_t pos = cleanType.find(w);
                        while (pos != std::string::npos)
                        {
                            cleanType.erase(pos, w.length());
                            pos = cleanType.find(w);
                        }
                    }
                    size_t start = cleanType.find_first_not_of(" \t");
                    size_t end = cleanType.find_last_not_of(" \t");
                    if (start != std::string::npos && end != std::string::npos)
                    {
                        cleanType = cleanType.substr(start, end - start + 1);
                    }

                    bool exists = (globalTable.FindGlobalByName(cleanType) != nullptr) ||
                                  (globalTable.FindFirst(cleanType) != nullptr) ||
                                  (localTable.FindGlobalByName(cleanType) != nullptr);
                    if (!exists)
                    {
                        TSPoint startPoint = ts_node_start_point(typeNode);
                        TSPoint endPoint = ts_node_end_point(typeNode);

                        lsp::Diagnostic d;
                        d.range.start.line = startPoint.row;
                        d.range.start.character = startPoint.column;
                        d.range.end.line = endPoint.row;
                        d.range.end.character = endPoint.column;
                        d.severity = lsp::DiagnosticSeverity::Error;
                        d.source = "angelscript";
                        d.message = fmt::format(fmt::runtime(strs.diagUndeclaredType), pTypeStr);
                        diags.push_back(d);
                    }
                }
            }

            // Extract default value node '=' EXPR
            TSNode valueNode = ts_node_child_by_field_name(child, "default", sizeof("default") - 1);
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
                sawDefault = true;
                if (!pTypeStr.empty())
                {
                    auto inferredOpt = TypeEvaluator::InferType(valueNode, doc, globalTable, localTable);
                    if (inferredOpt.has_value())
                    {
                        std::string defType = inferredOpt.value();
                        if (!TypeEvaluator::AreTypesCompatible(pTypeStr, defType))
                        {
                            TSPoint vStart = ts_node_start_point(valueNode);
                            TSPoint vEnd = ts_node_end_point(valueNode);

                            lsp::Diagnostic d;
                            d.range.start.line = vStart.row;
                            d.range.start.character = vStart.column;
                            d.range.end.line = vEnd.row;
                            d.range.end.character = vEnd.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = fmt::format(fmt::runtime(strs.diagDefaultParamTypeMismatch), defType, pTypeStr);
                            diags.push_back(d);
                        }
                    }
                }
            }
            else
            {
                if (sawDefault)
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
                    d.message = std::string(strs.diagDefaultParamOrder);
                    diags.push_back(d);
                }
            }
        }
    }

    std::vector<lsp::Diagnostic> FunctionValidator::ValidateFunction(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        // If function node is part of a funcdef declaration, skip Function validation
        TSNode pNode = ts_node_parent(node);
        if (!ts_node_is_null(pNode) && ts_node_type(pNode) && std::string(ts_node_type(pNode)) == "funcdef_declaration")
        {
            return diags;
        }

        const auto &strs = i18n::GetStrings(locale);

        // 1. Validate parameter list
        TSNode paramListNode = ts_node_child_by_field_name(node, "parameters", sizeof("parameters") - 1);
        if (ts_node_is_null(paramListNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "parameter_list" || std::string(cType) == "parameters"))
                {
                    paramListNode = child;
                    break;
                }
            }
        }
        ValidateParamList(paramListNode, doc, globalTable, localTable, locale, diags);

        // 2. Validate return type existence
        TSNode returnTypeNode = ts_node_child_by_field_name(node, "return_type", sizeof("return_type") - 1);
        if (ts_node_is_null(returnTypeNode))
        {
            returnTypeNode = ts_node_child_by_field_name(node, "type", sizeof("type") - 1);
        }

        std::string returnTypeStr = "void";
        if (!ts_node_is_null(returnTypeNode))
        {
            returnTypeStr = std::string(doc.SourceAt(returnTypeNode));
            if (!IsPrimitiveOrBuiltinType(returnTypeStr))
            {
                bool exists = (globalTable.FindGlobalByName(returnTypeStr) != nullptr) ||
                              (globalTable.FindFirst(returnTypeStr) != nullptr) ||
                              (localTable.FindGlobalByName(returnTypeStr) != nullptr);
                if (!exists)
                {
                    TSPoint start = ts_node_start_point(returnTypeNode);
                    TSPoint end = ts_node_end_point(returnTypeNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagUndeclaredType), returnTypeStr);
                    diags.push_back(d);
                }
            }
        }

        // 3. Validate global function attributes (override, property, final, explicit on top-level functions)
        bool isTopLevelGlobal = ts_node_is_null(pNode) ||
                                (ts_node_type(pNode) && std::string(ts_node_type(pNode)) == "script");
        if (isTopLevelGlobal)
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view attrText = doc.SourceAt(child);
                if (attrText == "override" || attrText == "property" || attrText == "final" || attrText == "explicit")
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
                    d.message = fmt::format(fmt::runtime(strs.diagInvalidFuncAttr), attrText);
                    diags.push_back(d);
                }
            }
        }

        // 4. Validate return statements in statement_block
        TSNode bodyNode = ts_node_child_by_field_name(node, "body", sizeof("body") - 1);
        if (ts_node_is_null(bodyNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "statement_block" || std::string(cType) == "compound_statement"))
                {
                    bodyNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(bodyNode))
        {
            auto walkBody = [&](auto self, TSNode bNode) -> void
            {
                const char *bTypeC = ts_node_type(bNode);
                std::string bType = bTypeC ? std::string(bTypeC) : "";

                if (bType == "return_statement")
                {
                    uint32_t retChildren = ts_node_child_count(bNode);
                    TSNode exprNode = {};
                    for (uint32_t r = 0; r < retChildren; ++r)
                    {
                        TSNode rChild = ts_node_child(bNode, r);
                        std::string_view rText = doc.SourceAt(rChild);
                        if (rText != "return" && rText != ";")
                        {
                            exprNode = rChild;
                            break;
                        }
                    }

                    if (returnTypeStr == "void")
                    {
                        if (!ts_node_is_null(exprNode))
                        {
                            TSPoint start = ts_node_start_point(bNode);
                            TSPoint end = ts_node_end_point(bNode);

                            lsp::Diagnostic d;
                            d.range.start.line = start.row;
                            d.range.start.character = start.column;
                            d.range.end.line = end.row;
                            d.range.end.character = end.column;
                            d.severity = lsp::DiagnosticSeverity::Error;
                            d.source = "angelscript";
                            d.message = std::string(strs.diagVoidReturnWithValue);
                            diags.push_back(d);
                        }
                    }
                    else
                    {
                        if (!ts_node_is_null(exprNode))
                        {
                            auto inferredOpt = TypeEvaluator::InferType(exprNode, doc, globalTable, localTable);
                            if (inferredOpt.has_value())
                            {
                                std::string actualType = inferredOpt.value();
                                if (!TypeEvaluator::AreTypesCompatible(returnTypeStr, actualType))
                                {
                                    TSPoint start = ts_node_start_point(exprNode);
                                    TSPoint end = ts_node_end_point(exprNode);

                                    lsp::Diagnostic d;
                                    d.range.start.line = start.row;
                                    d.range.start.character = start.column;
                                    d.range.end.line = end.row;
                                    d.range.end.character = end.column;
                                    d.severity = lsp::DiagnosticSeverity::Error;
                                    d.source = "angelscript";
                                    d.message = fmt::format(fmt::runtime(strs.diagReturnTypeMismatch), actualType, returnTypeStr);
                                    diags.push_back(d);
                                }
                            }
                        }
                    }
                }

                uint32_t bChildCount = ts_node_child_count(bNode);
                for (uint32_t c = 0; c < bChildCount; ++c)
                {
                    self(self, ts_node_child(bNode, c));
                }
            };

            walkBody(walkBody, bodyNode);
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> FunctionValidator::ValidateFuncdef(
        TSNode node,
        const Document &doc,
        const SymbolTable &globalTable,
        const SymbolTable &localTable,
        i18n::Locale locale)
    {
        std::vector<lsp::Diagnostic> diags;
        if (ts_node_is_null(node))
        {
            return diags;
        }

        // Validate parameter list for funcdef
        TSNode paramListNode = ts_node_child_by_field_name(node, "parameters", sizeof("parameters") - 1);
        if (ts_node_is_null(paramListNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "parameter_list" || std::string(cType) == "parameters"))
                {
                    paramListNode = child;
                    break;
                }
            }
        }
        ValidateParamList(paramListNode, doc, globalTable, localTable, locale, diags);

        return diags;
    }

} // namespace analysis::validators
