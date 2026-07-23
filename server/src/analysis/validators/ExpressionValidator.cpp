/**
 * @file ExpressionValidator.cpp
 * @brief Implementation of ExpressionValidator for expressions, casts, and assignments.
 * @ingroup Analysis
 */

#include "ExpressionValidator.h"
#include <spdlog/fmt/fmt.h>

namespace analysis::validators
{
    static bool IsNumericTypeString(std::string_view typeStr)
    {
        return typeStr == "int" || typeStr == "uint" || typeStr == "float" || typeStr == "double" ||
               typeStr == "int8" || typeStr == "int16" || typeStr == "int32" || typeStr == "int64" ||
               typeStr == "uint8" || typeStr == "uint16" || typeStr == "uint32" || typeStr == "uint64";
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateExpression(
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

        const auto &strs = i18n::GetStrings(locale);

        TSNode leftNode = ts_node_child_by_field_name(node, "left", sizeof("left") - 1);
        TSNode rightNode = ts_node_child_by_field_name(node, "right", sizeof("right") - 1);

        std::string opStr;
        uint32_t count = ts_node_child_count(node);
        if (count >= 3)
        {
            if (ts_node_is_null(leftNode))
            {
                leftNode = ts_node_child(node, 0);
            }
            if (ts_node_is_null(rightNode))
            {
                rightNode = ts_node_child(node, count - 1);
            }

            for (uint32_t i = 1; i < count - 1; ++i)
            {
                std::string_view text = doc.SourceAt(ts_node_child(node, i));
                if (text == "+" || text == "-" || text == "*" || text == "/" || text == "%" ||
                    text == "&&" || text == "||" || text == "and" || text == "or" ||
                    text == "is" || text == "!is")
                {
                    opStr = std::string(text);
                    break;
                }
            }
        }

        if (ts_node_is_null(leftNode) || ts_node_is_null(rightNode) || opStr.empty())
        {
            return diags;
        }

        auto leftTypeOpt = TypeEvaluator::InferType(leftNode, doc, globalTable, localTable);
        auto rightTypeOpt = TypeEvaluator::InferType(rightNode, doc, globalTable, localTable);

        std::string leftType = leftTypeOpt.value_or("");
        std::string rightType = rightTypeOpt.value_or("");

        if (opStr == "+" || opStr == "-" || opStr == "*" || opStr == "/" || opStr == "%")
        {
            if (!leftType.empty() && !rightType.empty())
            {
                bool leftNum = IsNumericTypeString(leftType);
                bool rightNum = IsNumericTypeString(rightType);

                if (opStr == "+" && (leftType == "string" || rightType == "string"))
                {
                    // Valid string concatenation
                }
                else if (!leftNum || !rightNum)
                {
                    TSPoint start = ts_node_start_point(node);
                    TSPoint end = ts_node_end_point(node);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagInvalidBinaryOperator), opStr, leftType, rightType);
                    diags.push_back(d);
                }
            }
        }
        else if (opStr == "&&" || opStr == "||" || opStr == "and" || opStr == "or")
        {
            if (!leftType.empty() && leftType != "bool")
            {
                TSPoint start = ts_node_start_point(leftNode);
                TSPoint end = ts_node_end_point(leftNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagInvalidLogicalOperand), opStr, leftType);
                diags.push_back(d);
            }
            else if (!rightType.empty() && rightType != "bool")
            {
                TSPoint start = ts_node_start_point(rightNode);
                TSPoint end = ts_node_end_point(rightNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagInvalidLogicalOperand), opStr, rightType);
                diags.push_back(d);
            }
        }
        else if (opStr == "is" || opStr == "!is")
        {
            if (IsNumericTypeString(leftType) || leftType == "bool")
            {
                TSPoint start = ts_node_start_point(node);
                TSPoint end = ts_node_end_point(node);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagInvalidHandleComparison), opStr);
                diags.push_back(d);
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateCast(
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

        const auto &strs = i18n::GetStrings(locale);

        TSNode targetTypeNode = ts_node_child_by_field_name(node, "type", sizeof("type") - 1);
        if (ts_node_is_null(targetTypeNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "type" || std::string(cType) == "primitive_type" || std::string(cType) == "identifier"))
                {
                    targetTypeNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(targetTypeNode))
        {
            std::string targetType = std::string(doc.SourceAt(targetTypeNode));

            // Check target type existence
            if (!IsNumericTypeString(targetType) && targetType != "bool" && targetType != "string" && targetType != "void")
            {
                bool exists = (globalTable.FindGlobalByName(targetType) != nullptr) ||
                              (globalTable.FindFirst(targetType) != nullptr) ||
                              (localTable.FindGlobalByName(targetType) != nullptr);
                if (!exists)
                {
                    TSPoint start = ts_node_start_point(targetTypeNode);
                    TSPoint end = ts_node_end_point(targetTypeNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagUndeclaredType), targetType);
                    diags.push_back(d);
                }
            }

            // Check source expression type and validity of cast
            TSNode exprChild = ts_node_child_by_field_name(node, "expression", sizeof("expression") - 1);
            if (ts_node_is_null(exprChild))
            {
                bool sawOpenParen = false;
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_child(node, i);
                    std::string_view text = doc.SourceAt(child);
                    if (text == "(")
                    {
                        sawOpenParen = true;
                        continue;
                    }
                    if (sawOpenParen && text != ")")
                    {
                        exprChild = child;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(exprChild))
            {
                auto sourceTypeOpt = TypeEvaluator::InferType(exprChild, doc, globalTable, localTable);
                if (sourceTypeOpt.has_value())
                {
                    std::string sourceType = sourceTypeOpt.value();
                    if (sourceType == "string" && IsNumericTypeString(targetType))
                    {
                        TSPoint start = ts_node_start_point(node);
                        TSPoint end = ts_node_end_point(node);

                        lsp::Diagnostic d;
                        d.range.start.line = start.row;
                        d.range.start.character = start.column;
                        d.range.end.line = end.row;
                        d.range.end.character = end.column;
                        d.severity = lsp::DiagnosticSeverity::Error;
                        d.source = "angelscript";
                        d.message = fmt::format(fmt::runtime(strs.diagInvalidCast), sourceType, targetType);
                        diags.push_back(d);
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateAssign(
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

        const auto &strs = i18n::GetStrings(locale);

        TSNode lhsNode = ts_node_child_by_field_name(node, "left", sizeof("left") - 1);
        if (ts_node_is_null(lhsNode))
        {
            lhsNode = ts_node_child(node, 0);
        }

        if (!ts_node_is_null(lhsNode))
        {
            std::string_view lhsText = doc.SourceAt(lhsNode);
            TSPoint start = ts_node_start_point(lhsNode);

            const Symbol *sym = localTable.FindLocalByNameAt(lhsText, start.row, start.column);
            if (!sym)
            {
                sym = localTable.FindLocalByName(lhsText);
            }
            if (!sym)
            {
                sym = globalTable.FindGlobalByName(lhsText);
            }

            if (sym && sym->typeInfo.find("const") != std::string::npos)
            {
                TSPoint end = ts_node_end_point(lhsNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagCannotAssignToConst), lhsText);
                diags.push_back(d);
            }
        }

        return diags;
    }

} // namespace analysis::validators
