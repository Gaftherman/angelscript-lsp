#include "analysis/SymbolCollector.h"
#include <string_view>

namespace analysis
{
    void SymbolCollector::CollectGlobals(const Document& doc, SymbolTable& table)
    {
        TSNode root = doc.RootNode();
        if (ts_node_is_null(root)) return;
        TraverseGlobals(root, doc, table, nullptr);
    }

    void SymbolCollector::TraverseGlobals(TSNode node, const Document& doc, SymbolTable& table, Symbol* parentScope)
    {
        if (ts_node_is_null(node)) return;

        std::string_view type = ts_node_type(node);

        if (type == "func_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Function;
            sym->fullRange = GetRange(node, doc);
            
            // Find identifier
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "identifier")
                {
                    sym->name = GetNodeText(child, doc);
                    sym->selectionRange = GetRange(child, doc);
                }
                else if (std::string_view(ts_node_type(child)) == "type")
                {
                    sym->typeInfo = GetNodeText(child, doc);
                }
            }
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }
            
            // Do not recurse into functions for globals
            return;
        }
        else if (type == "variable_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Variable;
            sym->fullRange = GetRange(node, doc);
            
            // Find type and declarator
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "type")
                {
                    sym->typeInfo = GetNodeText(child, doc);
                }
                else if (std::string_view(ts_node_type(child)) == "variable_declarator")
                {
                    // Find identifier inside declarator
                    for (uint32_t j = 0; j < ts_node_child_count(child); j++)
                    {
                        TSNode dchild = ts_node_child(child, j);
                        if (std::string_view(ts_node_type(dchild)) == "identifier")
                        {
                            sym->name = GetNodeText(dchild, doc);
                            sym->selectionRange = GetRange(dchild, doc);
                            break;
                        }
                    }
                }
            }
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }
            
            return;
        }

        else if (type == "class_declaration" || type == "namespace_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = (type == "class_declaration") ? SymbolKind::Class : SymbolKind::Namespace;
            sym->fullRange = GetRange(node, doc);
            
            TSNode bodyNode = {0};

            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view childType = ts_node_type(child);
                
                if (childType == "identifier" || childType == "scoped_identifier")
                {
                    sym->name = GetNodeText(child, doc);
                    sym->selectionRange = GetRange(child, doc);
                }
                else if (childType == "class_body" || childType == "namespace_body" || childType == "statement_block" || childType == "declaration_list")
                {
                    bodyNode = child;
                }
            }

            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }

            // Recurse into the body to find methods/properties, passing this symbol as parentScope
            if (!ts_node_is_null(bodyNode))
            {
                uint32_t bodyCount = ts_node_child_count(bodyNode);
                for (uint32_t i = 0; i < bodyCount; i++)
                {
                    TraverseGlobals(ts_node_child(bodyNode, i), doc, table, sym.get());
                }
            }
            
            return;
        }
        else if (type == "funcdef_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Funcdef;
            sym->fullRange = GetRange(node, doc);
            
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view childType = ts_node_type(child);
                if (childType == "identifier")
                {
                    sym->name = GetNodeText(child, doc);
                    sym->selectionRange = GetRange(child, doc);
                }
                else if (childType == "type" || childType == "return_type")
                {
                    sym->typeInfo = GetNodeText(child, doc);
                }
            }
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }
            return;
        }
        else if (type == "enum_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Enum;
            sym->fullRange = GetRange(node, doc);
            
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "identifier")
                {
                    sym->name = GetNodeText(child, doc);
                    sym->selectionRange = GetRange(child, doc);
                    break;
                }
            }
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }
            return;
        }
        else if (type == "virtual_property")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Property;
            sym->fullRange = GetRange(node, doc);
            
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view childType = ts_node_type(child);
                if (childType == "identifier" || childType == "name")
                {
                    sym->name = GetNodeText(child, doc);
                    sym->selectionRange = GetRange(child, doc);
                }
                else if (childType == "type" || childType == "prop_type")
                {
                    sym->typeInfo = GetNodeText(child, doc);
                }
            }
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            else
            {
                table.AddGlobal(sym);
            }
            return;
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_child(node, i);
            TraverseGlobals(child, doc, table, parentScope);
        }
    }

    void SymbolCollector::TraverseLocals(TSNode node, const Document& doc, SymbolTable& table, Symbol* currentScope)
    {
        if (ts_node_is_null(node)) return;

        std::string_view type = ts_node_type(node);

        if (type == "variable_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->kind = SymbolKind::Variable;
            sym->fullRange = GetRange(node, doc);
            
            // Find type and declarator
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "type")
                {
                    sym->typeInfo = GetNodeText(child, doc);
                }
                else if (std::string_view(ts_node_type(child)) == "variable_declarator")
                {
                    // Find identifier inside declarator
                    for (uint32_t j = 0; j < ts_node_child_count(child); j++)
                    {
                        TSNode dchild = ts_node_child(child, j);
                        if (std::string_view(ts_node_type(dchild)) == "identifier")
                        {
                            sym->name = GetNodeText(dchild, doc);
                            sym->selectionRange = GetRange(dchild, doc);
                            break;
                        }
                    }
                }
            }
            
            if (currentScope)
            {
                sym->parent = currentScope;
            }
            table.AddLocal(sym);
            // Don't recurse inside variable_declaration
            return;
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_child(node, i);
            TraverseLocals(child, doc, table, currentScope);
        }
    }

    std::string SymbolCollector::GetNodeText(TSNode node, const Document& doc)
    {
        std::string_view sv = doc.SourceAt(node);
        return std::string(sv.begin(), sv.end());
    }

    lsp::Range SymbolCollector::GetRange(TSNode node, const Document& doc)
    {
        TSPoint start = ts_node_start_point(node);
        TSPoint end = ts_node_end_point(node);
        
        lsp::Range r;
        r.start.line = start.row;
        r.start.character = start.column;
        r.end.line = end.row;
        r.end.character = end.column;
        return r;
    }
}
