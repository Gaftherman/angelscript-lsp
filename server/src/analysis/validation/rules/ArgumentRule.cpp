/**
 * @file ArgumentRule.cpp
 * @brief Implementation of ArgumentRule for checking parameter types and duplicate parameter names.
 * @ingroup Analysis
 */

#include "ArgumentRule.h"
#include "analysis/validation/ValidationUtils.h"
#include <set>

namespace analysis::validation
{
    std::vector<lsDiagnostic> ArgumentRule::run(const ValidationContext &ctx)
    {
        std::vector<lsDiagnostic> diags;
        if (ts_node_is_null(ctx.rootNode))
        {
            return diags;
        }

        InspectNode(ctx.rootNode, ctx, diags);
        return diags;
    }

    void ArgumentRule::ValidateParameterList(TSNode paramsNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(paramsNode))
        {
            return;
        }

        std::set<std::string> seenParamNames;
        uint32_t count = ts_node_child_count(paramsNode);

        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_child(paramsNode, i);
            const char *cTypeStr = ts_node_type(child);
            std::string cType = cTypeStr ? std::string(cTypeStr) : "";

            if (cType == "parameter")
            {
                TSNode typeNode = ValidationUtils::getChildByFieldName(child, "type");
                TSNode nameNode = ValidationUtils::getChildByFieldName(child, "name");

                if (ts_node_is_null(typeNode) || ts_node_is_null(nameNode))
                {
                    uint32_t subCount = ts_node_child_count(child);
                    for (uint32_t j = 0; j < subCount; ++j)
                    {
                        TSNode sub = ts_node_child(child, j);
                        const char *sStr = ts_node_type(sub);
                        std::string st = sStr ? std::string(sStr) : "";
                        if (ts_node_is_null(typeNode) && (st == "type" || st == "type_identifier" || st == "primitive_type"))
                        {
                            typeNode = sub;
                        }
                        else if (ts_node_is_null(nameNode) && st == "identifier")
                        {
                            nameNode = sub;
                        }
                    }
                }

                // 1. Verify parameter type exists
                if (!ts_node_is_null(typeNode))
                {
                    std::string paramType = ValidationUtils::getNodeText(typeNode, ctx.document);
                    if (!paramType.empty() && !ValidationUtils::isValidType(paramType, ctx))
                    {
                        lsRange range = ValidationUtils::tsNodeToLsRange(typeNode);
                        std::string msg = "Type '" + paramType + "' does not exist in current scope";
                        diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                    }
                }

                // 2. Check duplicate parameter names
                if (!ts_node_is_null(nameNode))
                {
                    std::string paramName = ValidationUtils::getNodeText(nameNode, ctx.document);
                    if (!paramName.empty())
                    {
                        if (seenParamNames.contains(paramName))
                        {
                            lsRange range = ValidationUtils::tsNodeToLsRange(nameNode);
                            std::string msg = "Duplicate parameter name '" + paramName + "' in function signature";
                            diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                        }
                        else
                        {
                            seenParamNames.insert(paramName);
                        }
                    }
                }
            }
        }
    }

    void ArgumentRule::InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        const char *nTypeStr = ts_node_type(node);
        std::string nType = nTypeStr ? std::string(nTypeStr) : "";

        if (nType == "function_declaration" || nType == "method_declaration")
        {
            TSNode paramsNode = ValidationUtils::getChildByFieldName(node, "parameters");
            if (ts_node_is_null(paramsNode))
            {
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode c = ts_node_child(node, i);
                    const char *ctStr = ts_node_type(c);
                    std::string ct = ctStr ? std::string(ctStr) : "";
                    if (ct == "parameters" || ct == "parameter_list")
                    {
                        paramsNode = c;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(paramsNode))
            {
                ValidateParameterList(paramsNode, ctx, diags);
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            InspectNode(ts_node_child(node, i), ctx, diags);
        }
    }
}
