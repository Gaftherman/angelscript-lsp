#include "features/document_symbol/DocumentSymbolHandler.h"
#include "parser/AngelScriptParser.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Converts a Tree-Sitter TSPoint to an LSP Position.
         * @param pt Tree-Sitter point with row and column.
         * @return Equivalent LSP Position.
         */
        inline lsp::Position ToLspPosition(TSPoint pt)
        {
            return lsp::Position{ pt.row, pt.column };
        }

        /**
         * @brief Converts a Tree-Sitter TSNode range to an LSP Range.
         * @param node Tree-Sitter AST node.
         * @return Enclosing LSP Range.
         */
        inline lsp::Range ToLspRange(TSNode node)
        {
            TSPoint start = ts_node_start_point(node);
            TSPoint end = ts_node_end_point(node);
            return lsp::Range{
                lsp::Position{ start.row, start.column },
                lsp::Position{ end.row, end.column }
            };
        }

        /**
         * @brief Extracts text content from source code for a given AST node.
         * @param node Target TSNode.
         * @param sourceCode Full document source text.
         * @return Extracted string slice.
         */
        inline std::string GetNodeText(TSNode node, const std::string &sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            if (start >= sourceCode.size() || end > sourceCode.size() || start >= end)
            {
                return "";
            }
            return sourceCode.substr(start, end - start);
        }

        /**
         * @brief Helper to query a child node by its grammar field name.
         * @param node Parent TSNode.
         * @param fieldName Field name in grammar.
         * @return Child TSNode.
         */
        inline TSNode GetChildByFieldName(TSNode node, const char *fieldName)
        {
            return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(std::strlen(fieldName)));
        }

        std::vector<lsp::DocumentSymbol> ProcessChildren(TSNode containerNode, const std::string &sourceCode, bool isInsideClass, std::string_view enclosingClassName);

        /**
         * @brief Recursively processes a single Tree-Sitter AST node and emits DocumentSymbols.
         * @param node Current AST node.
         * @param sourceCode Source text for identifier extraction.
         * @param isInsideClass True if current node is inside a class/interface body.
         * @param enclosingClassName Name of enclosing class if inside class.
         * @param outSymbols Output accumulator for created symbols.
         */
        void ProcessNode(TSNode node, const std::string &sourceCode, bool isInsideClass, std::string_view enclosingClassName, std::vector<lsp::DocumentSymbol> &outSymbols)
        {
            if (ts_node_is_null(node))
            {
                return;
            }

            const char *type = ts_node_type(node);
            if (!type)
            {
                return;
            }

            std::string_view nodeType(type);

            if (nodeType == "namespace_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Namespace;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "namespace";

                TSNode bodyNode = GetChildByFieldName(node, "body");
                if (!ts_node_is_null(bodyNode))
                {
                    auto children = ProcessChildren(bodyNode, sourceCode, false, "");
                    if (!children.empty())
                    {
                        sym.children = std::move(children);
                    }
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "class_declaration" || nodeType == "mixin_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                lsp::DocumentSymbol sym;
                sym.name = name;
                sym.kind = lsp::SymbolKind::Class;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = (nodeType == "mixin_declaration") ? "mixin" : "class";

                TSNode bodyNode = GetChildByFieldName(node, "body");
                if (!ts_node_is_null(bodyNode))
                {
                    auto children = ProcessChildren(bodyNode, sourceCode, true, name);
                    if (!children.empty())
                    {
                        sym.children = std::move(children);
                    }
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "interface_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                lsp::DocumentSymbol sym;
                sym.name = name;
                sym.kind = lsp::SymbolKind::Interface;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "interface";

                TSNode bodyNode = GetChildByFieldName(node, "body");
                if (!ts_node_is_null(bodyNode))
                {
                    auto children = ProcessChildren(bodyNode, sourceCode, true, name);
                    if (!children.empty())
                    {
                        sym.children = std::move(children);
                    }
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "enum_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Enum;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "enum";

                std::vector<lsp::DocumentSymbol> members;
                uint32_t childCount = ts_node_named_child_count(node);
                for (uint32_t i = 0; i < childCount; ++i)
                {
                    TSNode child = ts_node_named_child(node, i);
                    if (std::string_view(ts_node_type(child)) == "enum_member")
                    {
                        TSNode mNameNode = GetChildByFieldName(child, "name");
                        std::string mName = GetNodeText(mNameNode, sourceCode);
                        if (!mName.empty())
                        {
                            lsp::DocumentSymbol mSym;
                            mSym.name = std::move(mName);
                            mSym.kind = lsp::SymbolKind::EnumMember;
                            mSym.range = ToLspRange(child);
                            mSym.selectionRange = ts_node_is_null(mNameNode) ? mSym.range : ToLspRange(mNameNode);

                            TSNode valNode = GetChildByFieldName(child, "value");
                            if (!ts_node_is_null(valNode))
                            {
                                mSym.detail = "= " + GetNodeText(valNode, sourceCode);
                            }
                            members.push_back(std::move(mSym));
                        }
                    }
                }

                if (!members.empty())
                {
                    sym.children = std::move(members);
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "func_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                bool isDestructor = false;
                uint32_t cCount = ts_node_child_count(node);
                for (uint32_t i = 0; i < cCount; ++i)
                {
                    TSNode child = ts_node_child(node, i);
                    const char *cType = ts_node_type(child);
                    if (cType && std::strcmp(cType, "~") == 0)
                    {
                        isDestructor = true;
                        break;
                    }
                }

                TSNode retNode = GetChildByFieldName(node, "return_type");
                TSNode paramsNode = GetChildByFieldName(node, "parameters");

                lsp::DocumentSymbol sym;
                if (isDestructor)
                {
                    sym.name = "~" + name;
                    sym.kind = lsp::SymbolKind::Constructor;
                }
                else if (isInsideClass && name == enclosingClassName)
                {
                    sym.name = name;
                    sym.kind = lsp::SymbolKind::Constructor;
                }
                else
                {
                    sym.name = name;
                    sym.kind = isInsideClass ? lsp::SymbolKind::Method : lsp::SymbolKind::Function;
                }

                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);

                std::string retStr = GetNodeText(retNode, sourceCode);
                std::string paramsStr = GetNodeText(paramsNode, sourceCode);
                if (!retStr.empty() && !paramsStr.empty())
                {
                    sym.detail = retStr + " " + paramsStr;
                }
                else if (!retStr.empty())
                {
                    sym.detail = retStr;
                }
                else if (isDestructor || (isInsideClass && name == enclosingClassName))
                {
                    sym.detail = paramsStr;
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "interface_method")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                TSNode retNode = GetChildByFieldName(node, "return_type");
                TSNode paramsNode = GetChildByFieldName(node, "parameters");

                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Method;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);

                std::string retStr = GetNodeText(retNode, sourceCode);
                std::string paramsStr = GetNodeText(paramsNode, sourceCode);
                if (!retStr.empty() && !paramsStr.empty())
                {
                    sym.detail = retStr + " " + paramsStr;
                }
                else if (!retStr.empty())
                {
                    sym.detail = retStr;
                }

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "import_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Function;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "import";

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "variable_declaration")
            {
                TSNode typeNode = GetChildByFieldName(node, "var_type");
                std::string typeStr = GetNodeText(typeNode, sourceCode);

                uint32_t childCount = ts_node_named_child_count(node);
                uint32_t declaratorCount = 0;
                for (uint32_t i = 0; i < childCount; ++i)
                {
                    if (std::string_view(ts_node_type(ts_node_named_child(node, i))) == "variable_declarator")
                    {
                        declaratorCount++;
                    }
                }

                for (uint32_t i = 0; i < childCount; ++i)
                {
                    TSNode declChild = ts_node_named_child(node, i);
                    if (std::string_view(ts_node_type(declChild)) == "variable_declarator")
                    {
                        TSNode nameNode = GetChildByFieldName(declChild, "name");
                        std::string name = GetNodeText(nameNode, sourceCode);
                        if (!name.empty())
                        {
                            lsp::DocumentSymbol sym;
                            sym.name = std::move(name);
                            sym.kind = isInsideClass ? lsp::SymbolKind::Field : lsp::SymbolKind::Variable;
                            sym.range = (declaratorCount == 1) ? ToLspRange(node) : ToLspRange(declChild);
                            sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                            if (!typeStr.empty())
                            {
                                sym.detail = typeStr;
                            }
                            outSymbols.push_back(std::move(sym));
                        }
                    }
                }
            }
            else if (nodeType == "virtual_property")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                TSNode typeNode = GetChildByFieldName(node, "prop_type");
                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Property;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = GetNodeText(typeNode, sourceCode);

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "typedef_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                TSNode baseTypeNode = GetChildByFieldName(node, "base_type");
                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Class;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "typedef " + GetNodeText(baseTypeNode, sourceCode);

                outSymbols.push_back(std::move(sym));
            }
            else if (nodeType == "funcdef_declaration")
            {
                TSNode nameNode = GetChildByFieldName(node, "name");
                std::string name = GetNodeText(nameNode, sourceCode);
                if (name.empty())
                {
                    return;
                }

                TSNode retNode = GetChildByFieldName(node, "return_type");
                TSNode paramsNode = GetChildByFieldName(node, "parameters");

                lsp::DocumentSymbol sym;
                sym.name = std::move(name);
                sym.kind = lsp::SymbolKind::Function;
                sym.range = ToLspRange(node);
                sym.selectionRange = ts_node_is_null(nameNode) ? sym.range : ToLspRange(nameNode);
                sym.detail = "funcdef " + GetNodeText(retNode, sourceCode) + " " + GetNodeText(paramsNode, sourceCode);

                outSymbols.push_back(std::move(sym));
            }
        }

        /**
         * @brief Iterates child nodes of a container node (e.g. translation unit, namespace_body, class_body)
         *        and produces a vector of DocumentSymbol objects.
         * @param containerNode Container AST node.
         * @param sourceCode Document source text.
         * @param isInsideClass True if parent is a class/interface.
         * @param enclosingClassName Name of the enclosing class.
         * @return Vector of extracted child DocumentSymbol instances.
         */
        std::vector<lsp::DocumentSymbol> ProcessChildren(TSNode containerNode, const std::string &sourceCode, bool isInsideClass, std::string_view enclosingClassName)
        {
            std::vector<lsp::DocumentSymbol> symbols;
            uint32_t count = ts_node_named_child_count(containerNode);
            symbols.reserve(count);

            for (uint32_t i = 0; i < count; ++i)
            {
                TSNode child = ts_node_named_child(containerNode, i);
                ProcessNode(child, sourceCode, isInsideClass, enclosingClassName, symbols);
            }

            return symbols;
        }
    }

    std::optional<DocumentSymbolResult> GetDocumentSymbols(const DocumentSymbolRequest &request)
    {
        if (request.sourceCode.empty())
        {
            return DocumentSymbolResult{};
        }

        if (request.tree != nullptr)
        {
            TSNode root = ts_tree_root_node(request.tree);
            return ProcessChildren(root, request.sourceCode, false, "");
        }

        parser::AngelScriptParser parser;
        TSTree *tempTree = parser.Parse(request.sourceCode);
        if (!tempTree)
        {
            return DocumentSymbolResult{};
        }

        TSNode root = ts_tree_root_node(tempTree);
        auto result = ProcessChildren(root, request.sourceCode, false, "");
        ts_tree_delete(tempTree);
        return result;
    }
}
