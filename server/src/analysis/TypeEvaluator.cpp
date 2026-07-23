/**
 * @file TypeEvaluator.cpp
 * @brief Implementation of TypeEvaluator for type inference and type checking.
 * @ingroup Analysis
 */

#include "TypeEvaluator.h"
#include "analysis/SymbolResolver.h"
#include <algorithm>
#include <cctype>

namespace analysis
{
    static std::string StripTypeModifiers(std::string_view raw)
    {
        std::string s(raw);
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
        // Trim whitespace
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        if (start == std::string::npos)
        {
            return "";
        }
        return s.substr(start, end - start + 1);
    }

    static bool IsNumericType(std::string_view typeStr)
    {
        std::string clean = StripTypeModifiers(typeStr);
        return clean == "int" || clean == "uint" || clean == "float" || clean == "double" ||
               clean == "int8" || clean == "int16" || clean == "int32" || clean == "int64" ||
               clean == "uint8" || clean == "uint16" || clean == "uint32" || clean == "uint64";
    }

    std::optional<std::string> TypeEvaluator::InferType(TSNode exprNode, const Document &doc, const SymbolTable &globalTable, const SymbolTable &localTable)
    {
        if (ts_node_is_null(exprNode))
        {
            return std::nullopt;
        }

        const char *typeCStr = ts_node_type(exprNode);
        std::string nodeType = typeCStr ? std::string(typeCStr) : "";

        // Rule 3: Parenthesized Expressions
        if (nodeType == "parenthesized_expression" || nodeType == "expression")
        {
            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(exprNode, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) != "(" && std::string(cType) != ")"))
                {
                    return InferType(child, doc, globalTable, localTable);
                }
            }
        }

        // Rule 1: Literals
        if (nodeType == "literal" || nodeType == "number_literal" || nodeType == "string_literal" || nodeType == "boolean_literal")
        {
            std::string_view text = doc.SourceAt(exprNode);
            if (text == "true" || text == "false")
            {
                return "bool";
            }
            if (text.starts_with("\"") || text.starts_with("'"))
            {
                return "string";
            }
            if (text.find('.') != std::string_view::npos || text.ends_with('f') || text.ends_with('F'))
            {
                return "float";
            }
            return "int";
        }

        // Rule 2: Identifiers
        if (nodeType == "identifier")
        {
            std::string_view name = doc.SourceAt(exprNode);
            TSPoint start = ts_node_start_point(exprNode);

            const Symbol *sym = localTable.FindLocalByNameAt(name, start.row, start.column);
            if (!sym)
            {
                sym = localTable.FindLocalByName(name);
            }
            if (!sym)
            {
                sym = globalTable.FindGlobalByName(name);
            }
            if (!sym)
            {
                sym = globalTable.FindFirst(name);
            }

            if (sym && !sym->typeInfo.empty())
            {
                return StripTypeModifiers(sym->typeInfo);
            }
        }

        // Additional: If node is init_declarator / assignment expression, infer RHS
        if (nodeType == "init_declarator" || nodeType == "assignment_expression")
        {
            TSNode valueNode = ts_node_child_by_field_name(exprNode, "value", sizeof("value") - 1);
            if (ts_node_is_null(valueNode))
            {
                valueNode = ts_node_child_by_field_name(exprNode, "right", sizeof("right") - 1);
            }
            if (!ts_node_is_null(valueNode))
            {
                return InferType(valueNode, doc, globalTable, localTable);
            }
        }

        // Rule 4: Binary Expressions
        if (nodeType == "binary_expression" || nodeType == "math_expression" || nodeType == "logic_expression")
        {
            TSNode leftNode = ts_node_child_by_field_name(exprNode, "left", sizeof("left") - 1);
            TSNode rightNode = ts_node_child_by_field_name(exprNode, "right", sizeof("right") - 1);
            if (ts_node_is_null(leftNode) || ts_node_is_null(rightNode))
            {
                uint32_t count = ts_node_child_count(exprNode);
                if (count >= 3)
                {
                    leftNode = ts_node_child(exprNode, 0);
                    rightNode = ts_node_child(exprNode, count - 1);
                }
            }

            auto leftOpt = InferType(leftNode, doc, globalTable, localTable);
            auto rightOpt = InferType(rightNode, doc, globalTable, localTable);

            if (leftOpt.value_or("") == "string" || rightOpt.value_or("") == "string")
            {
                return "string";
            }

            uint32_t count = ts_node_child_count(exprNode);
            for (uint32_t i = 1; i < count; ++i)
            {
                TSNode opNode = ts_node_child(exprNode, i);
                std::string_view text = doc.SourceAt(opNode);
                if (text == "==" || text == "!=" || text == "<" || text == "<=" || text == ">" || text == ">=" ||
                    text == "&&" || text == "||" || text == "and" || text == "or" || text == "is" || text == "!is")
                {
                    return "bool";
                }
            }

            if (leftOpt.has_value())
            {
                return leftOpt.value();
            }
            return rightOpt;
        }

        // Rule 5: Cast Expressions
        if (nodeType == "cast" || nodeType == "cast_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", sizeof("type") - 1);
            if (!ts_node_is_null(typeNode))
            {
                return std::string(doc.SourceAt(typeNode));
            }
        }

        // Rule 6: Lambda Expressions
        if (nodeType == "lambda" || nodeType == "lambda_expression" || nodeType == "function_expression")
        {
            return "funcdef";
        }

        return std::nullopt;
    }

    bool TypeEvaluator::AreTypesCompatible(std::string_view targetType, std::string_view sourceType)
    {
        std::string targetClean = StripTypeModifiers(targetType);
        std::string sourceClean = StripTypeModifiers(sourceType);

        if (targetClean.empty() || sourceClean.empty())
        {
            return true;
        }

        if (targetClean == sourceClean)
        {
            return true;
        }

        // Exception for Phase 1: Implicit numeric conversions
        if (IsNumericType(targetClean) && IsNumericType(sourceClean))
        {
            return true;
        }

        return false;
    }

} // namespace analysis
