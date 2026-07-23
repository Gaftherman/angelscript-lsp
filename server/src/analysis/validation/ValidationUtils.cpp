/**
 * @file ValidationUtils.cpp
 * @brief Implementation of ValidationUtils helper functions.
 * @ingroup Analysis
 */

#include "ValidationUtils.h"
#include <algorithm>
#include <cctype>
#include <ankerl/unordered_dense.h>

namespace analysis::validation
{
    namespace ValidationUtils
    {
        lsPosition tsPointToLsPosition(TSPoint point)
        {
            lsPosition pos;
            pos.line = point.row;
            pos.character = point.column;
            return pos;
        }

        lsRange tsNodeToLsRange(TSNode node)
        {
            lsRange r;
            r.start = tsPointToLsPosition(ts_node_start_point(node));
            r.end = tsPointToLsPosition(ts_node_end_point(node));
            return r;
        }

        std::string getNodeText(TSNode node, const Document &doc)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }
            std::string_view sv = doc.SourceAt(node);
            return std::string(sv);
        }

        std::vector<TSNode> findChildrenByType(TSNode parent, const std::string &typeName)
        {
            std::vector<TSNode> results;
            if (ts_node_is_null(parent))
            {
                return results;
            }

            uint32_t count = ts_node_child_count(parent);
            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_child(parent, i);
                const char *cType = ts_node_type(child);
                if (cType && typeName == cType)
                {
                    results.push_back(child);
                }
            }
            return results;
        }

        TSNode getChildByFieldName(TSNode parent, const std::string &fieldName)
        {
            if (ts_node_is_null(parent))
            {
                return {};
            }
            return ts_node_child_by_field_name(parent, fieldName.c_str(), static_cast<uint32_t>(fieldName.length()));
        }

        static std::string StripTypeModifiers(std::string_view typeStr)
        {
            std::string s(typeStr);
            while (s.find("const") != std::string::npos)
            {
                size_t pos = s.find("const");
                s.erase(pos, 5);
            }
            while (s.find("&inout") != std::string::npos)
            {
                size_t pos = s.find("&inout");
                s.erase(pos, 6);
            }
            while (s.find("&out") != std::string::npos)
            {
                size_t pos = s.find("&out");
                s.erase(pos, 4);
            }
            while (s.find("&in") != std::string::npos)
            {
                size_t pos = s.find("&in");
                s.erase(pos, 3);
            }
            while (s.find("&") != std::string::npos)
            {
                size_t pos = s.find("&");
                s.erase(pos, 1);
            }
            while (s.find("@") != std::string::npos)
            {
                size_t pos = s.find("@");
                s.erase(pos, 1);
            }
            while (s.find("?") != std::string::npos)
            {
                size_t pos = s.find("?");
                s.erase(pos, 1);
            }
            while (s.find("[]") != std::string::npos)
            {
                size_t pos = s.find("[]");
                s.erase(pos, 2);
            }
            size_t first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return "";
            }
            size_t last = s.find_last_not_of(" \t\r\n");
            return s.substr(first, (last - first + 1));
        }

        static bool IsTypeDeclaredInAst(TSNode node, std::string_view targetName, const Document &doc)
        {
            if (ts_node_is_null(node))
            {
                return false;
            }

            const char *nTypeStr = ts_node_type(node);
            std::string nType = nTypeStr ? std::string(nTypeStr) : "";

            if (nType == "class_declaration" || nType == "struct_declaration" ||
                nType == "interface_declaration" || nType == "enum_declaration" ||
                nType == "typedef_declaration" || nType == "funcdef_declaration" ||
                nType == "mixin_declaration")
            {
                TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
                if (ts_node_is_null(nameNode))
                {
                    uint32_t cCount = ts_node_child_count(node);
                    for (uint32_t i = 0; i < cCount; ++i)
                    {
                        TSNode c = ts_node_child(node, i);
                        const char *ct = ts_node_type(c);
                        if (ct && (std::string(ct) == "identifier" || std::string(ct) == "type_identifier"))
                        {
                            nameNode = c;
                            break;
                        }
                    }
                }

                if (!ts_node_is_null(nameNode))
                {
                    if (doc.SourceAt(nameNode) == targetName)
                    {
                        return true;
                    }
                }
            }

            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                if (IsTypeDeclaredInAst(ts_node_child(node, i), targetName, doc))
                {
                    return true;
                }
            }
            return false;
        }

        bool isValidType(const std::string &typeName, const ValidationContext &ctx)
        {
            std::string clean = StripTypeModifiers(typeName);
            if (clean.empty())
            {
                return true;
            }

            std::string unqualified = clean;
            size_t nsIdx = clean.rfind("::");
            if (nsIdx != std::string::npos)
            {
                unqualified = clean.substr(nsIdx + 2);
            }

            // Primitive pre-defined types in AngelScript
            static const ankerl::unordered_dense::set<std::string> primitives = {
                "void", "int", "int8", "int16", "int32", "int64",
                "uint", "uint8", "uint16", "uint32", "uint64",
                "float", "double", "bool", "string", "auto", "dictionary", "array"
            };

            if (primitives.contains(clean) || primitives.contains(unqualified))
            {
                return true;
            }

            // Generic template array syntax like array<int>
            if (clean.starts_with("array<") && clean.ends_with(">"))
            {
                std::string inner = clean.substr(6, clean.length() - 7);
                return isValidType(inner, ctx);
            }

            // Check if declared in current AST
            if (IsTypeDeclaredInAst(ctx.rootNode, clean, ctx.document) ||
                IsTypeDeclaredInAst(ctx.rootNode, unqualified, ctx.document))
            {
                return true;
            }

            // Check in document SymbolTable
            const Symbol *sym = ctx.symbolTable.FindByNameDeep(clean);
            if (!sym) sym = ctx.symbolTable.FindFirst(clean);
            if (!sym) sym = ctx.symbolTable.FindByNameDeep(unqualified);
            if (!sym) sym = ctx.symbolTable.FindFirst(unqualified);

            // Check in Global SymbolTable
            if (!sym && ctx.globalTable)
            {
                sym = ctx.globalTable->FindByNameDeep(clean);
                if (!sym) sym = ctx.globalTable->FindFirst(clean);
                if (!sym) sym = ctx.globalTable->FindByNameDeep(unqualified);
                if (!sym) sym = ctx.globalTable->FindFirst(unqualified);
            }

            if (sym)
            {
                if (sym->kind == SymbolKind::Class ||
                    sym->kind == SymbolKind::Interface ||
                    sym->kind == SymbolKind::Enum ||
                    sym->kind == SymbolKind::Typedef ||
                    sym->kind == SymbolKind::Funcdef ||
                    sym->kind == SymbolKind::Mixin ||
                    sym->kind == SymbolKind::Namespace)
                {
                    return true;
                }
            }

            return false;
        }

        lsDiagnostic createDiagnostic(lsRange range, const std::string &message, lsDiagnosticSeverity severity)
        {
            lsDiagnostic diag;
            diag.range = range;
            diag.message = message;
            diag.severity = severity;
            diag.source = "angelscript";
            return diag;
        }
    }
}
