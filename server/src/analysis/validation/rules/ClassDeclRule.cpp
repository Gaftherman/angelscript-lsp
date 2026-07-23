/**
 * @file ClassDeclRule.cpp
 * @brief Implementation of ClassDeclRule for validating inheritance and duplicate class members.
 * @ingroup Analysis
 */

#include "ClassDeclRule.h"
#include "analysis/validation/ValidationUtils.h"
#include <set>

namespace analysis::validation
{
    std::vector<lsDiagnostic> ClassDeclRule::run(const ValidationContext &ctx)
    {
        std::vector<lsDiagnostic> diags;
        if (ts_node_is_null(ctx.rootNode))
        {
            return diags;
        }

        InspectNode(ctx.rootNode, ctx, diags);
        return diags;
    }

    void ClassDeclRule::CheckClassInheritance(TSNode classNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        TSNode baseNode = ValidationUtils::getChildByFieldName(classNode, "base");
        if (ts_node_is_null(baseNode))
        {
            uint32_t count = ts_node_child_count(classNode);
            bool foundColon = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(classNode, i);
                std::string text = ValidationUtils::getNodeText(child, ctx.document);
                if (text == ":")
                {
                    foundColon = true;
                    continue;
                }
                if (foundColon)
                {
                    const char *cTypeStr = ts_node_type(child);
                    std::string cType = cTypeStr ? std::string(cTypeStr) : "";
                    if (cType == "identifier" || cType == "type_identifier" || cType == "type")
                    {
                        baseNode = child;
                        break;
                    }
                }
            }
        }

        if (!ts_node_is_null(baseNode))
        {
            std::string baseClassName = ValidationUtils::getNodeText(baseNode, ctx.document);
            if (!baseClassName.empty())
            {
                const Symbol *sym = ctx.symbolTable.FindByNameDeep(baseClassName);
                if (!sym)
                {
                    sym = ctx.symbolTable.FindFirst(baseClassName);
                }
                if (!sym && ctx.globalTable)
                {
                    sym = ctx.globalTable->FindByNameDeep(baseClassName);
                    if (!sym)
                    {
                        sym = ctx.globalTable->FindFirst(baseClassName);
                    }
                }

                if (!sym)
                {
                    lsRange range = ValidationUtils::tsNodeToLsRange(baseNode);
                    std::string msg = "Base class '" + baseClassName + "' not found";
                    diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                }
            }
        }
    }

    void ClassDeclRule::CheckClassBodyMembers(TSNode bodyNode, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(bodyNode))
        {
            return;
        }

        std::set<std::string> seenMembers;
        uint32_t childCount = ts_node_child_count(bodyNode);

        for (uint32_t i = 0; i < childCount; ++i)
        {
            TSNode child = ts_node_child(bodyNode, i);
            const char *cTypeStr = ts_node_type(child);
            std::string cType = cTypeStr ? std::string(cTypeStr) : "";

            if (cType == "variable_declaration" || cType == "property_declaration" ||
                cType == "method_declaration" || cType == "function_declaration")
            {
                TSNode nameNode = ValidationUtils::getChildByFieldName(child, "name");
                if (ts_node_is_null(nameNode))
                {
                    uint32_t subCount = ts_node_child_count(child);
                    for (uint32_t j = 0; j < subCount; ++j)
                    {
                        TSNode sub = ts_node_child(child, j);
                        const char *stStr = ts_node_type(sub);
                        std::string st = stStr ? std::string(stStr) : "";
                        if (st == "identifier")
                        {
                            nameNode = sub;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(nameNode))
                {
                    std::string memberName = ValidationUtils::getNodeText(nameNode, ctx.document);
                    if (!memberName.empty())
                    {
                        if (seenMembers.contains(memberName))
                        {
                            lsRange range = ValidationUtils::tsNodeToLsRange(nameNode);
                            std::string msg = "Duplicate member '" + memberName + "' in class body";
                            diags.push_back(ValidationUtils::createDiagnostic(range, msg, lsDiagnosticSeverity::Error));
                        }
                        else
                        {
                            seenMembers.insert(memberName);
                        }
                    }
                }
            }
        }
    }

    void ClassDeclRule::InspectNode(TSNode node, const ValidationContext &ctx, std::vector<lsDiagnostic> &diags)
    {
        if (ts_node_is_null(node))
        {
            return;
        }

        const char *nTypeStr = ts_node_type(node);
        std::string nType = nTypeStr ? std::string(nTypeStr) : "";

        if (nType == "class_declaration" || nType == "struct_declaration")
        {
            CheckClassInheritance(node, ctx, diags);

            TSNode bodyNode = ValidationUtils::getChildByFieldName(node, "body");
            if (ts_node_is_null(bodyNode))
            {
                uint32_t count = ts_node_child_count(node);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode c = ts_node_child(node, i);
                    const char *ctStr = ts_node_type(c);
                    std::string ct = ctStr ? std::string(ctStr) : "";
                    if (ct == "class_body" || ct == "body")
                    {
                        bodyNode = c;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(bodyNode))
            {
                CheckClassBodyMembers(bodyNode, ctx, diags);
            }
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            InspectNode(ts_node_child(node, i), ctx, diags);
        }
    }
}
