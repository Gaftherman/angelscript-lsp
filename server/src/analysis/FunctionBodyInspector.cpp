#include "analysis/FunctionBodyInspector.h"
#include "analysis/TypeExtraction.h"
#include <tree_sitter/api.h>
#include <unordered_set>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <optional>

extern "C" const TSLanguage *tree_sitter_angelscript();

namespace angel_lsp::analysis
{
    #define SYM_NAME(str) str, static_cast<uint32_t>(sizeof(str) - 1)

    struct FlowTraversalContext
    {
        int loopDepth = 0;
        int switchDepth = 0;
    };

    static TSNode GetChildByFieldName(TSNode node, const char *fieldName)
    {
        return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(std::strlen(fieldName)));
    }

    static std::string ExtractTSNodeText(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return "";

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
            return "";

        return sourceCode.substr(start, end - start);
    }

    static std::string_view ExtractTSNodeView(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return {};

        uint32_t start = ts_node_start_byte(node);
        uint32_t end = ts_node_end_byte(node);
        if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
            return {};

        return std::string_view(sourceCode.data() + start, end - start);
    }

    static SourceRange GetSourceRange(TSNode node)
    {
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        return SourceRange{ start.row, start.column, end.row, end.column };
    }

    struct BodyInspectorSymbols
    {
        TSSymbol symForStatement = 0;
        TSSymbol symWhileStatement = 0;
        TSSymbol symDoWhileStatement = 0;
        TSSymbol symSwitchStatement = 0;
        TSSymbol symCaseClause = 0;
        TSSymbol symBreakStatement = 0;
        TSSymbol symContinueStatement = 0;
        TSSymbol symReturnStatement = 0;
        TSSymbol symNullLiteral = 0;
        TSSymbol symCallExpression = 0;
        TSSymbol symGotoStatement = 0;
        TSSymbol symCastExpression = 0;
        TSSymbol symScopedIdentifier = 0;
        TSSymbol symScopedType = 0;
        TSSymbol symVariableDeclaration = 0;
        TSSymbol symNumberLiteral = 0;
        TSSymbol symParenthesizedExpression = 0;
        TSSymbol symUnaryExpression = 0;
        TSSymbol symBinaryExpression = 0;
        TSSymbol symIdentifier = 0;
        TSSymbol symVariableDeclarator = 0;
        TSSymbol symMemberExpression = 0;
        TSSymbol symMemberAccess = 0;
        TSSymbol symParameter = 0;
        TSSymbol tokOpenParen = 0;
        TSSymbol tokCloseParen = 0;
        TSSymbol tokDefault = 0;

        BodyInspectorSymbols()
        {
            const TSLanguage *lang = tree_sitter_angelscript();
            symForStatement = ts_language_symbol_for_name(lang, SYM_NAME("for_statement"), true);
            symWhileStatement = ts_language_symbol_for_name(lang, SYM_NAME("while_statement"), true);
            symDoWhileStatement = ts_language_symbol_for_name(lang, SYM_NAME("do_while_statement"), true);
            symSwitchStatement = ts_language_symbol_for_name(lang, SYM_NAME("switch_statement"), true);
            symCaseClause = ts_language_symbol_for_name(lang, SYM_NAME("case_clause"), true);
            symBreakStatement = ts_language_symbol_for_name(lang, SYM_NAME("break_statement"), true);
            symContinueStatement = ts_language_symbol_for_name(lang, SYM_NAME("continue_statement"), true);
            symReturnStatement = ts_language_symbol_for_name(lang, SYM_NAME("return_statement"), true);
            symNullLiteral = ts_language_symbol_for_name(lang, SYM_NAME("null_literal"), true);
            symCallExpression = ts_language_symbol_for_name(lang, SYM_NAME("call_expression"), true);
            symGotoStatement = ts_language_symbol_for_name(lang, SYM_NAME("goto_statement"), true);
            symCastExpression = ts_language_symbol_for_name(lang, SYM_NAME("cast_expression"), true);
            symScopedIdentifier = ts_language_symbol_for_name(lang, SYM_NAME("scoped_identifier"), true);
            symScopedType = ts_language_symbol_for_name(lang, SYM_NAME("scoped_type"), true);
            symVariableDeclaration = ts_language_symbol_for_name(lang, SYM_NAME("variable_declaration"), true);
            symNumberLiteral = ts_language_symbol_for_name(lang, SYM_NAME("number_literal"), true);
            symParenthesizedExpression = ts_language_symbol_for_name(lang, SYM_NAME("parenthesized_expression"), true);
            symUnaryExpression = ts_language_symbol_for_name(lang, SYM_NAME("unary_expression"), true);
            symBinaryExpression = ts_language_symbol_for_name(lang, SYM_NAME("binary_expression"), true);
            symIdentifier = ts_language_symbol_for_name(lang, SYM_NAME("identifier"), true);
            symVariableDeclarator = ts_language_symbol_for_name(lang, SYM_NAME("variable_declarator"), true);
            symMemberExpression = ts_language_symbol_for_name(lang, SYM_NAME("member_expression"), true);
            symMemberAccess = ts_language_symbol_for_name(lang, SYM_NAME("member_access"), true);
            symParameter = ts_language_symbol_for_name(lang, SYM_NAME("parameter"), true);
            tokOpenParen = ts_language_symbol_for_name(lang, SYM_NAME("("), false);
            tokCloseParen = ts_language_symbol_for_name(lang, SYM_NAME(")"), false);
            tokDefault = ts_language_symbol_for_name(lang, SYM_NAME("default"), false);
        }
    };

    static const BodyInspectorSymbols &GetBodyInspectorSymbols()
    {
        static const BodyInspectorSymbols symbols;
        return symbols;
    }

    static std::optional<int64_t> EvaluateConstantIntExpr(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return std::nullopt;

        const auto &symbols = GetBodyInspectorSymbols();
        TSSymbol nodeSym = ts_node_symbol(node);
        if (nodeSym == symbols.symNumberLiteral)
        {
            std::string text = ExtractTSNodeText(node, sourceCode);
            if (text.find('.') != std::string::npos || text.find('f') != std::string::npos || text.find('F') != std::string::npos)
            {
                return std::nullopt;
            }
            try
            {
                if (text.starts_with("0x") || text.starts_with("0X"))
                {
                    return std::stoll(text.substr(2), nullptr, 16);
                }
                if (text.starts_with("0b") || text.starts_with("0B"))
                {
                    return std::stoll(text.substr(2), nullptr, 2);
                }
                return std::stoll(text);
            }
            catch (...)
            {
                return std::nullopt;
            }
        }
        else if (nodeSym == symbols.symParenthesizedExpression)
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                TSSymbol cSym = ts_node_symbol(child);
                if (cSym != symbols.tokOpenParen && cSym != symbols.tokCloseParen)
                {
                    return EvaluateConstantIntExpr(child, sourceCode);
                }
            }
        }
        else if (nodeSym == symbols.symUnaryExpression)
        {
            TSNode opNode = GetChildByFieldName(node, "operand");
            TSNode operatorNode = GetChildByFieldName(node, "operator");
            std::string_view opStr = ExtractTSNodeView(operatorNode, sourceCode);
            auto val = EvaluateConstantIntExpr(opNode, sourceCode);
            if (val.has_value())
            {
                if (opStr == "-") return (*val != INT64_MIN) ? std::optional<int64_t>(-(*val)) : std::nullopt;
                if (opStr == "+") return +(*val);
                if (opStr == "~") return ~(*val);
                if (opStr == "!") return !(*val);
            }
        }
        else if (nodeSym == symbols.symBinaryExpression)
        {
            TSNode leftNode = GetChildByFieldName(node, "left");
            TSNode rightNode = GetChildByFieldName(node, "right");
            TSNode operatorNode = GetChildByFieldName(node, "operator");
            std::string_view opStr = ExtractTSNodeView(operatorNode, sourceCode);

            auto leftVal = EvaluateConstantIntExpr(leftNode, sourceCode);
            auto rightVal = EvaluateConstantIntExpr(rightNode, sourceCode);
            if (leftVal.has_value() && rightVal.has_value())
            {
                int64_t l = *leftVal;
                int64_t r = *rightVal;
                if (opStr == "+") return l + r;
                if (opStr == "-") return l - r;
                if (opStr == "*") return l * r;
                if (opStr == "/") return (r != 0) ? std::optional<int64_t>(l / r) : std::nullopt;
                if (opStr == "%") return (r != 0) ? std::optional<int64_t>(l % r) : std::nullopt;
                if (opStr == "<<") return (r >= 0 && r < 64) ? std::optional<int64_t>(l << r) : std::nullopt;
                if (opStr == ">>") return (r >= 0 && r < 64) ? std::optional<int64_t>(l >> r) : std::nullopt;
                if (opStr == "&") return l & r;
                if (opStr == "|") return l | r;
                if (opStr == "^") return l ^ r;
            }
        }

        return std::nullopt;
    }

    static bool IsConstantIntegerExpression(TSNode node, const std::string &sourceCode)
    {
        if (ts_node_is_null(node))
            return false;

        if (EvaluateConstantIntExpr(node, sourceCode).has_value())
        {
            return true;
        }

        const auto &symbols = GetBodyInspectorSymbols();
        TSSymbol nodeSym = ts_node_symbol(node);
        if (nodeSym == symbols.symIdentifier || nodeSym == symbols.symScopedIdentifier)
        {
            return true;
        }
        if (nodeSym == symbols.symUnaryExpression)
        {
            TSNode operand = GetChildByFieldName(node, "operand");
            return IsConstantIntegerExpression(operand, sourceCode);
        }
        if (nodeSym == symbols.symBinaryExpression)
        {
            TSNode left = GetChildByFieldName(node, "left");
            TSNode right = GetChildByFieldName(node, "right");
            return IsConstantIntegerExpression(left, sourceCode) && IsConstantIntegerExpression(right, sourceCode);
        }
        if (nodeSym == symbols.symParenthesizedExpression)
        {
            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(node, i);
                TSSymbol cSym = ts_node_symbol(child);
                if (cSym != symbols.tokOpenParen && cSym != symbols.tokCloseParen)
                {
                    return IsConstantIntegerExpression(child, sourceCode);
                }
            }
        }
        return false;
    }

    static void InspectFunctionBodyASTInternal(TSNode node, FunctionBodyAnalysis &analysis, const std::string &sourceCode, FlowTraversalContext flowCtx, int depth)
    {
        if (ts_node_is_null(node))
            return;
        if (depth >= 256)
            return;

        const auto &symbols = GetBodyInspectorSymbols();
        TSSymbol sym = ts_node_symbol(node);

        FlowTraversalContext childCtx = flowCtx;
        if (sym == symbols.symForStatement ||
            sym == symbols.symWhileStatement ||
            sym == symbols.symDoWhileStatement)
        {
            childCtx.loopDepth++;
        }
        else if (sym == symbols.symSwitchStatement)
        {
            childCtx.switchDepth++;

            std::unordered_set<std::string> seenCaseValues;
            bool seenDefault = false;

            uint32_t cCount = ts_node_child_count(node);
            for (uint32_t i = 0; i < cCount; ++i)
            {
                TSNode child = ts_node_child(node, i);
                if (!ts_node_is_null(child) && ts_node_symbol(child) == symbols.symCaseClause)
                {
                    TSNode firstChild = ts_node_child(child, 0);
                    bool isDefaultClause = (!ts_node_is_null(firstChild) && ts_node_symbol(firstChild) == symbols.tokDefault);

                    if (isDefaultClause)
                    {
                        if (seenDefault)
                        {
                            analysis.invalidDefaultStatements.push_back(GetSourceRange(child));
                        }
                        seenDefault = true;
                    }
                    else
                    {
                        if (seenDefault)
                        {
                            analysis.invalidDefaultStatements.push_back(GetSourceRange(child));
                        }

                        TSNode valNode = GetChildByFieldName(child, "value");

                        if (!ts_node_is_null(valNode))
                        {
                            std::string vText = ExtractTSNodeText(valNode, sourceCode);
                            bool isValidType = IsConstantIntegerExpression(valNode, sourceCode);

                            if (!isValidType)
                            {
                                analysis.invalidCaseStatements.push_back({ GetSourceRange(valNode), vText, "invalid_type" });
                            }
                            else
                            {
                                std::string normValue = vText;
                                auto evalOpt = EvaluateConstantIntExpr(valNode, sourceCode);
                                if (evalOpt.has_value())
                                {
                                    normValue = std::to_string(*evalOpt);
                                }

                                if (seenCaseValues.contains(normValue))
                                {
                                    analysis.invalidCaseStatements.push_back({ GetSourceRange(valNode), vText, "duplicate_value" });
                                }
                                else
                                {
                                    seenCaseValues.insert(normValue);
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (sym == symbols.symBreakStatement)
        {
            if (flowCtx.loopDepth == 0 && flowCtx.switchDepth == 0)
            {
                analysis.invalidBreakStatements.push_back(GetSourceRange(node));
            }
        }
        else if (sym == symbols.symContinueStatement)
        {
            if (flowCtx.loopDepth == 0)
            {
                analysis.invalidContinueStatements.push_back(GetSourceRange(node));
            }
        }

        if (sym == symbols.symReturnStatement)
        {
            TSNode valNode = GetChildByFieldName(node, "value");
            if (ts_node_is_null(valNode))
            {
                analysis.hasEmptyReturn = true;
            }
            else
            {
                analysis.hasValueReturn = true;
                analysis.returnExpression = ExtractTSNodeText(valNode, sourceCode);
                if (ts_node_symbol(valNode) == symbols.symNullLiteral)
                {
                    analysis.hasNullReturn = true;
                }
                else if (ts_node_symbol(valNode) == symbols.symCallExpression)
                {
                    TSNode funcNode = GetChildByFieldName(valNode, "function");
                    if (!ts_node_is_null(funcNode))
                    {
                        analysis.returnCallTargetName = ExtractTSNodeText(funcNode, sourceCode);
                    }
                }
            }
        }

        if (sym == symbols.symCallExpression)
        {
            TSNode funcChild = GetChildByFieldName(node, "function");
            if (!ts_node_is_null(funcChild))
            {
                std::string_view fText = ExtractTSNodeView(funcChild, sourceCode);
                if (fText == "super")
                {
                    analysis.hasSuperCall = true;
                }
            }
        }
        else if (sym == symbols.symGotoStatement)
        {
            TSNode labelChild = GetChildByFieldName(node, "label");
            if (ts_node_is_null(labelChild))
            {
                labelChild = GetChildByFieldName(node, "name");
            }
            if (!ts_node_is_null(labelChild))
            {
                std::string lName = ExtractTSNodeText(labelChild, sourceCode);
                if (!lName.empty())
                {
                    analysis.gotoTargetLabels.push_back(lName);
                }
            }
        }
        else if (sym == symbols.symCastExpression)
        {
            TSNode typeChild = GetChildByFieldName(node, "type");
            TSNode valueChild = GetChildByFieldName(node, "value");
            if (ts_node_is_null(valueChild)) valueChild = GetChildByFieldName(node, "expression");
            if (ts_node_is_null(valueChild) && ts_node_named_child_count(node) >= 2)
            {
                valueChild = ts_node_named_child(node, 1);
            }
            if (!ts_node_is_null(typeChild))
            {
                std::string cType = ExtractTSNodeText(typeChild, sourceCode);
                std::string opText = !ts_node_is_null(valueChild) ? ExtractTSNodeText(valueChild, sourceCode) : "";
                if (!cType.empty())
                {
                    analysis.bodyCastExpressions.push_back({ cType, opText, ExtractTypeInfoFromAST(typeChild, sourceCode) });
                }
            }
        }
        else if (sym == symbols.symScopedIdentifier || sym == symbols.symScopedType)
        {
            TSNode parentNode = ts_node_parent(node);
            TSSymbol parentSym = ts_node_is_null(parentNode) ? 0 : ts_node_symbol(parentNode);
            if (parentSym != symbols.symScopedIdentifier && parentSym != symbols.symScopedType)
            {
                std::string qName = ExtractTSNodeText(node, sourceCode);
                if (!qName.empty() && qName.find("::") != std::string::npos)
                {
                    analysis.bodyQualifiedNames.push_back(qName);
                }
            }
        }
        else if (sym == symbols.symVariableDeclaration)
        {
            TSNode typeNode = GetChildByFieldName(node, "var_type");
            std::string varName;
            TSNode declNode = GetChildByFieldName(node, "declarator");
            if (ts_node_is_null(declNode)) declNode = GetChildByFieldName(node, "name");
            if (ts_node_is_null(declNode))
            {
                uint32_t cCount = ts_node_named_child_count(node);
                for (uint32_t c = 0; c < cCount; ++c)
                {
                    TSNode child = ts_node_named_child(node, c);
                    TSSymbol cSym = ts_node_symbol(child);
                    if (cSym == symbols.symVariableDeclarator || cSym == symbols.symIdentifier)
                    {
                        declNode = child;
                        break;
                    }
                }
            }
            if (!ts_node_is_null(declNode))
            {
                varName = ExtractTSNodeText(declNode, sourceCode);
                size_t eqPos = varName.find('=');
                if (eqPos != std::string::npos) varName = varName.substr(0, eqPos);
                while (!varName.empty() && (varName.back() == '@' || varName.back() == '&' || isspace(static_cast<unsigned char>(varName.back()))))
                {
                    varName.pop_back();
                }
            }

            if (!ts_node_is_null(typeNode))
            {
                std::string fullVType = ExtractTSNodeText(typeNode, sourceCode);
                std::string vType = fullVType;
                while (!vType.empty() && (vType.back() == '@' || vType.back() == '&' || isspace(static_cast<unsigned char>(vType.back()))))
                {
                    vType.pop_back();
                }
                size_t constPos = vType.find("const ");
                if (constPos != std::string::npos)
                {
                    vType = vType.substr(constPos + 6);
                }
                size_t anglePos = vType.find('<');
                if (anglePos != std::string::npos)
                {
                    vType = vType.substr(0, anglePos);
                }
                if (!vType.empty())
                {
                    std::string initText;
                    TSNode initNode = GetChildByFieldName(node, "value");
                    if (ts_node_is_null(initNode)) initNode = GetChildByFieldName(node, "initializer");
                    if (ts_node_is_null(initNode) && !ts_node_is_null(declNode))
                    {
                        initNode = GetChildByFieldName(declNode, "value");
                        if (ts_node_is_null(initNode)) initNode = GetChildByFieldName(declNode, "initializer");
                    }
                    if (!ts_node_is_null(initNode))
                    {
                        initText = ExtractTSNodeText(initNode, sourceCode);
                    }
                    else if (!ts_node_is_null(declNode))
                    {
                        std::string declText = ExtractTSNodeText(declNode, sourceCode);
                        size_t eqPos = declText.find('=');
                        if (eqPos != std::string::npos)
                        {
                            initText = declText.substr(eqPos + 1);
                            while (!initText.empty() && isspace(static_cast<unsigned char>(initText.front())))
                                initText.erase(initText.begin());
                        }
                    }

                    analysis.bodyVariableTypes.push_back({ GetSourceRange(node), vType, varName, fullVType, initText });
                }
            }
        }

        if (sym == symbols.symIdentifier)
        {
            TSNode parentNode = ts_node_parent(node);
            bool isDecl = false;
            bool isMemberAccess = false;
            bool isCall = false;

            if (!ts_node_is_null(parentNode))
            {
                TSSymbol pSym = ts_node_symbol(parentNode);
                if (pSym == symbols.symVariableDeclarator || pSym == symbols.symVariableDeclaration || pSym == symbols.symParameter)
                {
                    TSNode declName = GetChildByFieldName(parentNode, "name");
                    if (ts_node_is_null(declName))
                    {
                        declName = GetChildByFieldName(parentNode, "declarator");
                    }
                    if (!ts_node_is_null(declName) && ts_node_eq(declName, node))
                    {
                        isDecl = true;
                    }
                    else if (pSym == symbols.symVariableDeclarator)
                    {
                        if (ts_node_child_count(parentNode) > 0 && ts_node_eq(ts_node_child(parentNode, 0), node))
                        {
                            isDecl = true;
                        }
                    }
                }
                else if (pSym == symbols.symMemberExpression || pSym == symbols.symMemberAccess)
                {
                    TSNode memberChild = GetChildByFieldName(parentNode, "member");
                    if (!ts_node_is_null(memberChild) && ts_node_eq(memberChild, node))
                    {
                        isMemberAccess = true;
                    }
                    else if (ts_node_child_count(parentNode) >= 2 && ts_node_eq(ts_node_child(parentNode, ts_node_child_count(parentNode) - 1), node))
                    {
                        isMemberAccess = true;
                    }
                }

                if (pSym == symbols.symCallExpression)
                {
                    TSNode funcChild = GetChildByFieldName(parentNode, "function");
                    if (!ts_node_is_null(funcChild) && ts_node_eq(funcChild, node))
                    {
                        isCall = true;
                    }
                }
                else
                {
                    TSNode grandParent = ts_node_parent(parentNode);
                    if (!ts_node_is_null(grandParent) && ts_node_symbol(grandParent) == symbols.symCallExpression)
                    {
                        TSNode funcChild = GetChildByFieldName(grandParent, "function");
                        if (!ts_node_is_null(funcChild) && ts_node_eq(funcChild, parentNode))
                        {
                            isCall = true;
                        }
                    }
                }
            }

            if (!isDecl)
            {
                std::string idText = ExtractTSNodeText(node, sourceCode);
                if (!idText.empty())
                {
                    analysis.bodyIdentifierRefs.push_back({ idText, GetSourceRange(node), isCall, isMemberAccess });
                }
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            InspectFunctionBodyASTInternal(ts_node_child(node, i), analysis, sourceCode, childCtx, depth + 1);
        }
    }

    void InspectFunctionBodyAST(TSNode bodyNode, FunctionBodyAnalysis &analysis, const std::string &sourceCode)
    {
        InspectFunctionBodyASTInternal(bodyNode, analysis, sourceCode, {}, 0);
    }
}
