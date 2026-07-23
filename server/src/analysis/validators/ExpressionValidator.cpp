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
            std::string cleanLhs(lhsText);
            if (cleanLhs.starts_with("@"))
            {
                cleanLhs = cleanLhs.substr(1);
            }
            TSPoint start = ts_node_start_point(lhsNode);

            const Symbol *sym = localTable.FindLocalByNameAt(cleanLhs, start.row, start.column);
            if (!sym)
            {
                sym = localTable.FindLocalByName(cleanLhs);
            }
            if (!sym)
            {
                sym = globalTable.FindGlobalByName(cleanLhs);
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

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateMemberAccess(
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

        TSNode objNode = ts_node_child_by_field_name(node, "object", sizeof("object") - 1);
        TSNode memNode = ts_node_child_by_field_name(node, "member", sizeof("member") - 1);
        if (ts_node_is_null(memNode))
        {
            memNode = ts_node_child_by_field_name(node, "field", sizeof("field") - 1);
        }

        if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                std::string_view t = doc.SourceAt(ts_node_child(node, i));
                if (t == ".")
                {
                    if (i > 0) objNode = ts_node_child(node, i - 1);
                    if (i + 1 < count) memNode = ts_node_child(node, i + 1);
                    break;
                }
            }
        }

        if (!ts_node_is_null(objNode) && !ts_node_is_null(memNode))
        {
            auto objTypeOpt = TypeEvaluator::InferType(objNode, doc, globalTable, localTable);
            if (objTypeOpt.has_value())
            {
                std::string objType = objTypeOpt.value();
                if (objType.ends_with("@"))
                {
                    objType.pop_back();
                }
                std::string cleanClassName = objType;
                if (cleanClassName.starts_with("const "))
                {
                    cleanClassName = cleanClassName.substr(6);
                }

                SymbolTable combined = localTable;
                combined.MergeGlobals(globalTable);

                const Symbol *classSym = combined.FindByNameDeep(cleanClassName);
                if (!classSym)
                {
                    classSym = combined.FindFirst(cleanClassName);
                }

                if (classSym)
                {
                    std::string memName = std::string(doc.SourceAt(memNode));
                    for (const auto &child : classSym->children)
                    {
                        if (child->name == memName && (child->kind == SymbolKind::Method || child->kind == SymbolKind::Function))
                        {
                            bool objIsConst = objTypeOpt.value().starts_with("const ");
                            if (!objIsConst)
                            {
                                std::string_view objText = doc.SourceAt(objNode);
                                TSPoint oStart = ts_node_start_point(objNode);
                                const Symbol *objSym = localTable.FindLocalByNameAt(objText, oStart.row, oStart.column);
                                if (!objSym) objSym = localTable.FindLocalByName(objText);
                                if (!objSym) objSym = globalTable.FindGlobalByName(objText);
                                if (objSym && objSym->typeInfo.starts_with("const "))
                                {
                                    objIsConst = true;
                                }
                            }

                            bool childIsConst = child->isConstMethod;
                            if (objIsConst && !childIsConst)
                            {
                                TSPoint mStart = ts_node_start_point(memNode);
                                TSPoint mEnd = ts_node_end_point(memNode);

                                lsp::Diagnostic d;
                                d.range.start.line = mStart.row;
                                d.range.start.character = mStart.column;
                                d.range.end.line = mEnd.row;
                                d.range.end.character = mEnd.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = fmt::format(fmt::runtime(strs.diagCannotAssignToConst), memName);
                                diags.push_back(d);
                            }
                        }

                        if (child->name == memName && child->isPrivate)
                        {
                            TSPoint start = ts_node_start_point(node);
                            const Symbol *currentScope = combined.FindScopeByPosition(doc.GetUri(), start.row, start.column);
                            bool insideClass = false;
                            while (currentScope)
                            {
                                if (currentScope->name == classSym->name && currentScope->kind == SymbolKind::Class)
                                {
                                    insideClass = true;
                                    break;
                                }
                                currentScope = currentScope->parent;
                            }

                            if (!insideClass)
                            {
                                TSPoint mStart = ts_node_start_point(memNode);
                                TSPoint mEnd = ts_node_end_point(memNode);

                                lsp::Diagnostic d;
                                d.range.start.line = mStart.row;
                                d.range.start.character = mStart.column;
                                d.range.end.line = mEnd.row;
                                d.range.end.character = mEnd.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = fmt::format(fmt::runtime(strs.diagPrivateMemberAccess), memName, classSym->name);
                                diags.push_back(d);
                            }
                            break;
                        }
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateCallArguments(
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

        const char *nodeTypeStr = ts_node_type(node);
        if (nodeTypeStr && std::string(nodeTypeStr) == "expression_statement")
        {
            for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "call_expression" || std::string(cType) == "function_call"))
                {
                    node = child;
                    break;
                }
            }
        }

        const auto &strs = i18n::GetStrings(locale);

        TSNode funcNameNode = ts_node_child_by_field_name(node, "function", sizeof("function") - 1);
        if (ts_node_is_null(funcNameNode))
        {
            funcNameNode = ts_node_child(node, 0);
        }

        if (!ts_node_is_null(funcNameNode))
        {
            std::string funcName = std::string(doc.SourceAt(funcNameNode));
            size_t parenPos = funcName.find('(');
            if (parenPos != std::string::npos)
            {
                funcName = funcName.substr(0, parenPos);
            }
            size_t startIdx = funcName.find_first_not_of(" \t\r\n");
            size_t endIdx = funcName.find_last_not_of(" \t\r\n");
            if (startIdx != std::string::npos && endIdx != std::string::npos)
            {
                funcName = funcName.substr(startIdx, endIdx - startIdx + 1);
            }

            SymbolTable combined = localTable;
            combined.MergeGlobals(globalTable);

            const Symbol *funcSym = combined.FindByNameDeep(funcName);
            if (!funcSym)
            {
                funcSym = combined.FindFirst(funcName);
            }

            if (funcSym && funcSym->kind == SymbolKind::Function)
            {
                TSNode argListNode = ts_node_child_by_field_name(node, "arguments", sizeof("arguments") - 1);
                if (ts_node_is_null(argListNode))
                {
                    uint32_t count = ts_node_child_count(node);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        TSNode child = ts_node_child(node, i);
                        const char *cType = ts_node_type(child);
                        if (cType && (std::string(cType) == "argument_list" || std::string(cType) == "arguments"))
                        {
                            argListNode = child;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(argListNode))
                {
                    std::vector<TSNode> args;
                    uint32_t aCount = ts_node_child_count(argListNode);
                    for (uint32_t i = 0; i < aCount; ++i)
                    {
                        TSNode aChild = ts_node_child(argListNode, i);
                        std::string_view aText = doc.SourceAt(aChild);
                        if (aText != "(" && aText != ")" && aText != ",")
                        {
                            args.push_back(aChild);
                        }
                    }

                    std::vector<const Symbol *> params;
                    for (const auto &c : funcSym->children)
                    {
                        if (c->kind == SymbolKind::Parameter)
                        {
                            params.push_back(c.get());
                        }
                    }

                    for (size_t i = 0; i < args.size() && i < params.size(); ++i)
                    {
                        const auto *paramSym = params[i];
                        TSNode argNode = args[i];
                        std::string_view aText = doc.SourceAt(argNode);
                        if (aText.find("function") != std::string_view::npos)
                        {
                            std::string targetType = paramSym->typeInfo;
                            if (targetType.ends_with("@"))
                            {
                                targetType.pop_back();
                            }

                            const Symbol *funcdefSym = combined.FindByNameDeep(targetType);
                            if (!funcdefSym)
                            {
                                funcdefSym = combined.FindFirst(targetType);
                            }

                            if (funcdefSym && funcdefSym->kind == SymbolKind::Funcdef)
                            {
                                std::vector<std::string> expectedParamTypes;
                                for (const auto &p : funcdefSym->params)
                                {
                                    expectedParamTypes.push_back(p.typeName);
                                }

                                std::vector<std::string> lambdaParamTypes;
                                TSNode pListNode = ts_node_child_by_field_name(argNode, "parameters", sizeof("parameters") - 1);
                                if (ts_node_is_null(pListNode))
                                {
                                    uint32_t lCount = ts_node_child_count(argNode);
                                    for (uint32_t l = 0; l < lCount; ++l)
                                    {
                                        TSNode lChild = ts_node_child(argNode, l);
                                        const char *lcType = ts_node_type(lChild);
                                        if (lcType && (std::string(lcType) == "parameter_list" || std::string(lcType) == "parameters"))
                                        {
                                            pListNode = lChild;
                                            break;
                                        }
                                    }
                                }

                                if (!ts_node_is_null(pListNode))
                                {
                                    uint32_t pCount = ts_node_child_count(pListNode);
                                    for (uint32_t p = 0; p < pCount; ++p)
                                    {
                                        TSNode paramNode = ts_node_child(pListNode, p);
                                        const char *pcType = ts_node_type(paramNode);
                                        if (pcType && std::string(pcType) == "parameter")
                                        {
                                            TSNode pTypeNode = ts_node_child_by_field_name(paramNode, "type", sizeof("type") - 1);
                                            if (ts_node_is_null(pTypeNode))
                                            {
                                                pTypeNode = ts_node_child(paramNode, 0);
                                            }
                                            if (!ts_node_is_null(pTypeNode))
                                            {
                                                lambdaParamTypes.push_back(std::string(doc.SourceAt(pTypeNode)));
                                            }
                                        }
                                    }
                                }

                                bool mismatch = (expectedParamTypes.size() != lambdaParamTypes.size());
                                if (!mismatch)
                                {
                                    for (size_t k = 0; k < expectedParamTypes.size(); ++k)
                                    {
                                        if (expectedParamTypes[k] != lambdaParamTypes[k])
                                        {
                                            mismatch = true;
                                            break;
                                        }
                                    }
                                }

                                if (mismatch)
                                {
                                    TSPoint start = ts_node_start_point(argNode);
                                    TSPoint end = ts_node_end_point(argNode);

                                    lsp::Diagnostic d;
                                    d.range.start.line = start.row;
                                    d.range.start.character = start.column;
                                    d.range.end.line = end.row;
                                    d.range.end.character = end.column;
                                    d.severity = lsp::DiagnosticSeverity::Error;
                                    d.source = "angelscript";
                                    d.message = fmt::format(fmt::runtime(strs.diagLambdaSignatureMismatch), targetType, "funcdef", "lambda");
                                    diags.push_back(d);
                                }
                            }
                        }

                        if (paramSym->typeInfo.find("&out") != std::string::npos ||
                            paramSym->typeInfo.find("&inout") != std::string::npos)
                        {
                            const char *argType = ts_node_type(argNode);
                            std::string aTypeStr = argType ? std::string(argType) : "";

                            bool isLiteral = (aTypeStr == "number_literal" || aTypeStr == "integer_literal" ||
                                              aTypeStr == "float_literal" || aTypeStr == "string_literal" ||
                                              aTypeStr == "boolean_literal" ||
                                              (!aText.empty() && (std::isdigit(aText[0]) || aText[0] == '"' || aText[0] == '\'')));

                            if (isLiteral)
                            {
                                TSPoint start = ts_node_start_point(argNode);
                                TSPoint end = ts_node_end_point(argNode);

                                lsp::Diagnostic d;
                                d.range.start.line = start.row;
                                d.range.start.character = start.column;
                                d.range.end.line = end.row;
                                d.range.end.character = end.column;
                                d.severity = lsp::DiagnosticSeverity::Error;
                                d.source = "angelscript";
                                d.message = std::string(strs.diagLValueRequired);
                                diags.push_back(d);
                            }
                        }
                    }
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateIncrementDecrement(
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

        TSNode operandNode = {0};
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            std::string_view text = doc.SourceAt(child);
            if (text != "++" && text != "--")
            {
                operandNode = child;
                break;
            }
        }

        if (!ts_node_is_null(operandNode))
        {
            const char *opType = ts_node_type(operandNode);
            std::string oTypeStr = opType ? std::string(opType) : "";
            std::string varName = std::string(doc.SourceAt(operandNode));
            size_t p = varName.find("++");
            if (p != std::string::npos)
            {
                varName.erase(p, 2);
            }
            p = varName.find("--");
            if (p != std::string::npos)
            {
                varName.erase(p, 2);
            }
            size_t sIdx = varName.find_first_not_of(" \t\r\n");
            size_t eIdx = varName.find_last_not_of(" \t\r\n");
            if (sIdx != std::string::npos && eIdx != std::string::npos)
            {
                varName = varName.substr(sIdx, eIdx - sIdx + 1);
            }

            bool isLiteral = (oTypeStr == "number_literal" || oTypeStr == "integer_literal" ||
                              oTypeStr == "float_literal" || oTypeStr == "string_literal" ||
                              (!varName.empty() && std::isdigit(varName[0])));

            if (isLiteral)
            {
                TSPoint start = ts_node_start_point(operandNode);
                TSPoint end = ts_node_end_point(operandNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = std::string(strs.diagInvalidIncrementOperand);
                diags.push_back(d);
            }
            else
            {
                TSPoint start = ts_node_start_point(operandNode);
                SymbolTable combined = localTable;
                combined.MergeGlobals(globalTable);

                const Symbol *sym = localTable.FindLocalByNameAt(varName, start.row, start.column);
                if (!sym)
                {
                    sym = combined.FindByNameDeep(varName);
                }
                if (!sym)
                {
                    sym = combined.FindFirst(varName);
                }

                if (sym && (sym->isConstMethod || sym->typeInfo.find("const") != std::string::npos))
                {
                    TSPoint end = ts_node_end_point(operandNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagCannotAssignToConst), varName);
                    diags.push_back(d);
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateLambda(
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

        TSNode typeNode = ts_node_child_by_field_name(node, "type", sizeof("type") - 1);
        TSNode lambdaNode = {0};

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            const char *cType = ts_node_type(child);
            if (cType && std::string(cType) == "lambda_expression")
            {
                lambdaNode = child;
            }
            else if (cType && (std::string(cType) == "variable_declarator" || std::string(cType) == "init_declarator"))
            {
                uint32_t subCount = ts_node_child_count(child);
                for (uint32_t j = 0; j < subCount; ++j)
                {
                    TSNode subChild = ts_node_child(child, j);
                    const char *scType = ts_node_type(subChild);
                    if (scType && std::string(scType) == "lambda_expression")
                    {
                        lambdaNode = subChild;
                        break;
                    }
                }
            }
        }

        if (ts_node_is_null(lambdaNode))
        {
            return diags;
        }

        if (ts_node_is_null(typeNode))
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                const char *cType = ts_node_type(child);
                if (cType && (std::string(cType) == "type" || std::string(cType) == "primitive_type" || std::string(cType) == "identifier"))
                {
                    typeNode = child;
                    break;
                }
            }
        }

        if (!ts_node_is_null(typeNode))
        {
            std::string targetType = std::string(doc.SourceAt(typeNode));
            if (targetType.ends_with("@"))
            {
                targetType.pop_back();
            }

            SymbolTable combined = localTable;
            combined.MergeGlobals(globalTable);

            const Symbol *funcdefSym = combined.FindByNameDeep(targetType);
            if (!funcdefSym)
            {
                funcdefSym = combined.FindFirst(targetType);
            }

            if (!funcdefSym || funcdefSym->kind != SymbolKind::Funcdef)
            {
                TSPoint start = ts_node_start_point(lambdaNode);
                TSPoint end = ts_node_end_point(lambdaNode);

                lsp::Diagnostic d;
                d.range.start.line = start.row;
                d.range.start.character = start.column;
                d.range.end.line = end.row;
                d.range.end.character = end.column;
                d.severity = lsp::DiagnosticSeverity::Error;
                d.source = "angelscript";
                d.message = fmt::format(fmt::runtime(strs.diagLambdaNoMatchingFuncdef), targetType);
                diags.push_back(d);
            }
            else
            {
                TSNode paramsNode = ts_node_child_by_field_name(lambdaNode, "parameters", sizeof("parameters") - 1);
                if (ts_node_is_null(paramsNode))
                {
                    uint32_t lCount = ts_node_child_count(lambdaNode);
                    for (uint32_t i = 0; i < lCount; ++i)
                    {
                        TSNode lChild = ts_node_child(lambdaNode, i);
                        const char *lcType = ts_node_type(lChild);
                        if (lcType && (std::string(lcType) == "parameter_list" || std::string(lcType) == "parameters"))
                        {
                            paramsNode = lChild;
                            break;
                        }
                    }
                }

                std::vector<std::string> lambdaParamTypes;
                if (!ts_node_is_null(paramsNode))
                {
                    uint32_t pCount = ts_node_child_count(paramsNode);
                    for (uint32_t i = 0; i < pCount; ++i)
                    {
                        TSNode pChild = ts_node_child(paramsNode, i);
                        const char *pcType = ts_node_type(pChild);
                        if (pcType && std::string(pcType) == "parameter")
                        {
                            TSNode pTypeNode = ts_node_child_by_field_name(pChild, "type", sizeof("type") - 1);
                            if (ts_node_is_null(pTypeNode))
                            {
                                pTypeNode = ts_node_child(pChild, 0);
                            }
                            if (!ts_node_is_null(pTypeNode))
                            {
                                lambdaParamTypes.push_back(std::string(doc.SourceAt(pTypeNode)));
                            }
                        }
                    }
                }

                bool mismatch = false;
                std::vector<std::string> expectedParamTypes;
                for (const auto &p : funcdefSym->params)
                {
                    expectedParamTypes.push_back(p.typeName);
                }
                if (expectedParamTypes.empty())
                {
                    for (const auto &child : funcdefSym->children)
                    {
                        if (child->kind == SymbolKind::Parameter)
                        {
                            expectedParamTypes.push_back(child->typeInfo);
                        }
                    }
                }

                if (lambdaParamTypes.size() != expectedParamTypes.size())
                {
                    mismatch = true;
                }
                else
                {
                    auto cleanType = [](std::string t) {
                        if (t.starts_with("const ")) t = t.substr(6);
                        if (t.ends_with("@")) t.pop_back();
                        if (t.ends_with("&in")) t = t.substr(0, t.length() - 3);
                        if (t.ends_with("&out")) t = t.substr(0, t.length() - 4);
                        if (t.ends_with("&inout")) t = t.substr(0, t.length() - 6);
                        size_t s = t.find_first_not_of(" \t");
                        size_t e = t.find_last_not_of(" \t");
                        if (s != std::string::npos && e != std::string::npos) t = t.substr(s, e - s + 1);
                        return t;
                    };

                    for (size_t i = 0; i < lambdaParamTypes.size(); ++i)
                    {
                        std::string expectedType = expectedParamTypes[i];
                        std::string actualType = lambdaParamTypes[i];

                        if (cleanType(expectedType) != cleanType(actualType))
                        {
                            mismatch = true;
                            break;
                        }
                    }
                }

                if (mismatch)
                {
                    std::string expectedStr = "(";
                    for (size_t i = 0; i < expectedParamTypes.size(); ++i)
                    {
                        if (i > 0) expectedStr += ", ";
                        expectedStr += expectedParamTypes[i];
                    }
                    expectedStr += ")";

                    std::string actualStr = "(";
                    for (size_t i = 0; i < lambdaParamTypes.size(); ++i)
                    {
                        if (i > 0) actualStr += ", ";
                        actualStr += lambdaParamTypes[i];
                    }
                    actualStr += ")";

                    TSPoint start = ts_node_start_point(lambdaNode);
                    TSPoint end = ts_node_end_point(lambdaNode);

                    lsp::Diagnostic d;
                    d.range.start.line = start.row;
                    d.range.start.character = start.column;
                    d.range.end.line = end.row;
                    d.range.end.character = end.column;
                    d.severity = lsp::DiagnosticSeverity::Error;
                    d.source = "angelscript";
                    d.message = fmt::format(fmt::runtime(strs.diagLambdaSignatureMismatch), targetType, expectedStr, actualStr);
                    diags.push_back(d);
                }
            }
        }

        return diags;
    }

    std::vector<lsp::Diagnostic> ExpressionValidator::ValidateTernary(
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

        TSNode trueNode = {0};
        TSNode falseNode = {0};

        uint32_t count = ts_node_child_count(node);
        bool sawQ = false;
        bool sawC = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(node, i);
            std::string_view text = doc.SourceAt(child);
            if (text == "?")
            {
                sawQ = true;
                continue;
            }
            if (text == ":")
            {
                sawC = true;
                continue;
            }
            if (sawQ && !sawC && ts_node_is_null(trueNode))
            {
                trueNode = child;
            }
            if (sawC && ts_node_is_null(falseNode))
            {
                falseNode = child;
            }
        }

        if (!ts_node_is_null(trueNode) && !ts_node_is_null(falseNode))
        {
            auto type1Opt = TypeEvaluator::InferType(trueNode, doc, globalTable, localTable);
            auto type2Opt = TypeEvaluator::InferType(falseNode, doc, globalTable, localTable);
            if (type1Opt.has_value() && type2Opt.has_value())
            {
                std::string type1 = type1Opt.value();
                std::string type2 = type2Opt.value();

                SymbolTable combined = localTable;
                combined.MergeGlobals(globalTable);

                bool c1 = TypeEvaluator::AreTypesCompatible(type1, type2, &combined);
                bool c2 = TypeEvaluator::AreTypesCompatible(type2, type1, &combined);
                if (!c1 && !c2)
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
                    d.message = fmt::format(fmt::runtime(strs.diagTernaryTypeMismatch), type1, type2);
                    diags.push_back(d);
                }
            }
        }

        return diags;
    }

} // namespace analysis::validators
