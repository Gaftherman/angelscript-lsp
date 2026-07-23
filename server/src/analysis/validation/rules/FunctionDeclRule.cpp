/**
 * @file FunctionDeclRule.cpp
 * @brief Implementation of FunctionDeclRule for checking void return statements and function signature duplicates.
 * @ingroup Analysis
 */

#include "FunctionDeclRule.h"
#include "analysis/validation/ValidationUtils.h"

namespace analysis::validation
{
    std::vector<lsDiagnostic> FunctionDeclRule::run(const ValidationContext &ctx)
    {
        std::vector<lsDiagnostic> diags;
        if (ts_node_is_null(ctx.rootNode))
        {
            return diags;
        }

        std::set<std::string> seenSignatures;
        InspectNode(ctx.rootNode, ctx, diags, seenSignatures);
        return diags;
    }

    void FunctionDeclRule::CheckVoidReturns(TSNode bodyNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(bodyNode))
        {
            return;
        }

        const char *nTypeStr = ts_node_type(bodyNode);
        std::string nType = nTypeStr ? std::string(nTypeStr) : "";

        if (nType == "return_statement")
        {
            uint32_t cCount = ts_node_child_count(bodyNode);
            bool hasReturnExpr = false;
            TSNode exprNode{};

            for (uint32_t i = 0; i < cCount; ++i)
            {
                TSNode c = ts_node_child(bodyNode, i);
                std::string text = ValidationUtils::getNodeText(c, ctx.document);
                if (text != "return" && text != ";")
                {
                    hasReturnExpr = true;
                    exprNode = c;
                    break;
                }
            }

            if (hasReturnExpr && !ts_node_is_null(exprNode))
            {
                lsRange range = ValidationUtils::tsNodeToLsRange(exprNode);
                std::string msg = "Cannot return a value from a function returning void";
                diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
            }
        }

        uint32_t count = ts_node_child_count(bodyNode);
        for (uint32_t i = 0; i < count; ++i)
        {
            CheckVoidReturns(ts_node_child(bodyNode, i), ctx, diags);
        }
    }

    void FunctionDeclRule::InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags, std::set<std::string> &seenSignatures)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        const char *nTypeStr = ts_node_type(node);
        std::string nType = nTypeStr ? std::string(nTypeStr) : "";

        if (nType == "function_declaration" || nType == "method_declaration")
        {
            TSNode nameNode = ValidationUtils::getChildByFieldName(node, "name");
            TSNode typeNode = ValidationUtils::getChildByFieldName(node, "type");
            TSNode paramsNode = ValidationUtils::getChildByFieldName(node, "parameters");
            TSNode bodyNode = ValidationUtils::getChildByFieldName(node, "body");

            std::string retType = ValidationUtils::getNodeText(typeNode, ctx.document);
            std::string funcName = ValidationUtils::getNodeText(nameNode, ctx.document);

            // Check void returns
            if (retType == "void" && !ts_node_is_null(bodyNode))
            {
                CheckVoidReturns(bodyNode, ctx, diags);
            }

            // Check duplicate function declarations in scope
            if (!funcName.empty())
            {
                std::string paramsStr = ValidationUtils::getNodeText(paramsNode, ctx.document);
                std::string signatureKey = funcName + paramsStr;

                if (seenSignatures.contains(signatureKey))
                {
                    lsRange range = ValidationUtils::tsNodeToLsRange(nameNode.id ? nameNode : node);
                    std::string msg = "Redefinition of function '" + funcName + "' with identical signature";
                    diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                }
                else
                {
                    seenSignatures.insert(signatureKey);
                }
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            InspectNode(ts_node_child(node, i), ctx, diags, seenSignatures);
        }
    }
}
