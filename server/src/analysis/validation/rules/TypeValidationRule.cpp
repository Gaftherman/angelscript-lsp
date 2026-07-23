/**
 * @file TypeValidationRule.cpp
 * @brief Implementation of TypeValidationRule for checking type existence in AST declarations.
 * @ingroup Analysis
 */

#include "TypeValidationRule.h"
#include "analysis/validation/ValidationUtils.h"

namespace analysis::validation
{
    std::vector<lsDiagnostic> TypeValidationRule::run(const ValidationContext &ctx)
    {
        std::vector<lsDiagnostic> diags;
        if (ts_node_is_null(ctx.rootNode))
        {
            return diags;
        }

        InspectNode(ctx.rootNode, ctx, diags);
        return diags;
    }

    void TypeValidationRule::CheckTypeNode(TSNode typeNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(typeNode))
        {
            return;
        }

        std::string typeName = ValidationUtils::getNodeText(typeNode, ctx.document);
        if (typeName.empty())
        {
            return;
        }

        if (!ValidationUtils::isValidType(typeName, ctx))
        {
            lsRange range = ValidationUtils::tsNodeToLsRange(typeNode);
            std::string msg = "Type '" + typeName + "' does not exist in current scope";
            diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
        }
    }

    void TypeValidationRule::InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        const char *nTypeStr = ts_node_type(node);
        std::string nType = nTypeStr ? std::string(nTypeStr) : "";

        if (nType == "function_declaration" || nType == "method_declaration" ||
            nType == "variable_declaration" || nType == "parameter" ||
            nType == "property_declaration")
        {
            TSNode typeNode = ValidationUtils::getChildByFieldName(node, "type");
            if (ts_node_is_null(typeNode))
            {
                uint32_t cCount = ts_node_child_count(node);
                for (uint32_t i = 0; i < cCount; ++i)
                {
                    TSNode c = ts_node_child(node, i);
                    const char *ct = ts_node_type(c);
                    if (ct && (std::string(ct) == "type" || std::string(ct) == "primitive_type" || std::string(ct) == "scoped_type"))
                    {
                        typeNode = c;
                        break;
                    }
                }
            }

            CheckTypeNode(typeNode, ctx, diags);
        }
        else if (nType == "call_expression" || nType == "function_call" || nType == "call")
        {
            TSNode callee = ValidationUtils::getChildByFieldName(node, "function");
            if (ts_node_is_null(callee))
            {
                callee = ts_node_child(node, 0);
            }
            if (!ts_node_is_null(callee) && std::string(ts_node_type(callee)) == "identifier")
            {
                std::string funcName = ValidationUtils::getNodeText(callee, ctx.document);
                static const ankerl::unordered_dense::set<std::string> builtins = {
                    "Print", "log", "assert", "cast"
                };

                if (!funcName.empty() && !builtins.contains(funcName))
                {
                    const Symbol *sym = ctx.symbolTable.FindByNameDeep(funcName);
                    if (!sym) sym = ctx.symbolTable.FindFirst(funcName);
                    if (!sym && ctx.globalTable)
                    {
                        sym = ctx.globalTable->FindByNameDeep(funcName);
                        if (!sym) sym = ctx.globalTable->FindFirst(funcName);
                    }

                    if (!sym && !ValidationUtils::isValidType(funcName, ctx))
                    {
                        lsRange range = ValidationUtils::tsNodeToLsRange(callee);
                        std::string msg = "Undeclared identifier or symbol: '" + funcName + "'";
                        diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                    }
                }
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            InspectNode(ts_node_child(node, i), ctx, diags);
        }
    }
}
