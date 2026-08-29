#include "analysis/NamespaceChecker.h"
#include "analysis/SemanticHelpers.h"

#include <cctype>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
    void CheckNamespacesAndScopes(const NamespaceCheckRequest &request, DiagnosticContext &ctx)
    {
        const TSNode root = request.root;
        const std::string_view sourceCode = request.sourceCode;

        const auto &table = ctx.request.symbolTable;
        auto usings = CollectUsingNamespaces(root, sourceCode);

        std::vector<TSNode> stack = { root };
        while (!stack.empty())
        {
            TSNode node = stack.back();
            stack.pop_back();

            std::string_view type = ts_node_type(node);

            if (type == "scoped_identifier")
            {
                uint32_t childCount = ts_node_child_count(node);
                if (childCount >= 3)
                {
                    TSNode firstChild = ts_node_child(node, 0);
                    std::string_view firstType = ts_node_type(firstChild);
                    if (firstType == "identifier")
                    {
                        std::string prefix = GetNodeText(firstChild, sourceCode);
                        while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.front()))) prefix.erase(prefix.begin());
                        while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.back()))) prefix.pop_back();

                        if (!prefix.empty() && !IsKnownScope(prefix, node, sourceCode, table))
                        {
                            TSPoint startPt = ts_node_start_point(firstChild);
                            TSPoint endPt = ts_node_end_point(firstChild);
                            ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                            "as-err-undefined-namespace", prefix, DiagnosticSeverity::Error);
                        }
                    }
                }
            }

            if (type == "call_expression")
            {
                TSNode funcNode = ts_node_child_by_field_name(node, "function", 8);
                if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
                {
                    funcNode = ts_node_child(node, 0);
                }

                if (!ts_node_is_null(funcNode))
                {
                    std::string calleeName = GetNodeText(funcNode, sourceCode);
                    while (!calleeName.empty() && isspace(static_cast<unsigned char>(calleeName.front()))) calleeName.erase(calleeName.begin());
                    while (!calleeName.empty() && isspace(static_cast<unsigned char>(calleeName.back()))) calleeName.pop_back();

                    if (!calleeName.empty() && calleeName.find("::") == std::string::npos && calleeName.find('.') == std::string::npos)
                    {
                        const Scope *scope = ctx.request.scopeRoot ? FindEnclosingScope(ctx.request.scopeRoot.get(), ts_node_start_point(node).row, ts_node_start_point(node).column) : nullptr;
                        const LocalDefinition *localDef = scope ? ResolveInScope(scope, calleeName) : nullptr;
                        auto inScopeSyms = FindSymbolsInScope(calleeName, node, sourceCode, table);
                        if (!localDef && inScopeSyms.empty() && !ctx.request.IsRegisteredSymbol(calleeName) && !table.HasSymbol(calleeName))
                        {
                            TSPoint startPt = ts_node_start_point(funcNode);
                            TSPoint endPt = ts_node_end_point(funcNode);
                            ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                            "as-err-undefined-identifier", calleeName, DiagnosticSeverity::Error);
                        }
                        else
                        {
                            auto containers = GetEnclosingContainers(node, sourceCode);
                            auto directSyms = FindSymbolsInScope(table, containers, calleeName, {});
                            if (directSyms.empty())
                            {
                                ankerl::unordered_dense::set<std::string> matchingNamespaces;
                                for (const auto &ns : usings)
                                {
                                    std::string q = ns + "::" + calleeName;
                                    if (!table.FindSymbols(q).empty())
                                    {
                                        matchingNamespaces.insert(ns);
                                    }
                                }

                                if (matchingNamespaces.size() > 1)
                                {
                                    TSPoint startPt = ts_node_start_point(funcNode);
                                    TSPoint endPt = ts_node_end_point(funcNode);
                                    ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                                    "as-err-ambiguous-identifier", calleeName, DiagnosticSeverity::Error);
                                }
                            }
                        }
                    }
                }
            }

            if (type == "import_declaration" || type == "ERROR")
            {
                std::string nodeText = GetNodeText(node, sourceCode);
                size_t importPos = nodeText.find("import ");
                if (importPos != std::string::npos)
                {
                    size_t fromPos = nodeText.find("from", importPos);
                    if (fromPos != std::string::npos && nodeText.find('{', fromPos) != std::string::npos)
                    {
                        TSPoint startPt = ts_node_start_point(node);
                        TSPoint endPt = ts_node_end_point(node);
                        ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column,
                                        "as-err-import-has-body", "import", DiagnosticSeverity::Error);
                    }
                }
            }

            uint32_t count = ts_node_child_count(node);
            for (uint32_t i = 0; i < count; ++i)
            {
                stack.push_back(ts_node_child(node, i));
            }
        }
    }
}
