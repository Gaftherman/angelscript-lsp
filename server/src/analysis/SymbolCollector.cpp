#include "analysis/SymbolCollector.h"
#include <string_view>
#include <sstream>
#include <iostream>

namespace analysis
{
    std::string SymbolCollector::GetNodeText(TSNode node, const Document& doc)
    {
        if (ts_node_is_null(node)) return "";
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

    static void ReadParams(TSNode paramListNode, const Document& doc, Symbol& sym, SymbolTable* table = nullptr, Symbol* parentFunc = nullptr)
    {
        if (ts_node_is_null(paramListNode)) return;
        
        for (uint32_t i = 0; i < ts_node_child_count(paramListNode); i++)
        {
            TSNode child = ts_node_child(paramListNode, i);
            if (std::string_view(ts_node_type(child)) != "parameter") continue;
            
            TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
            
            SymbolParam param;
            if (!ts_node_is_null(nameNode)) param.name = SymbolCollector::GetNodeText(nameNode, doc);
            
            // Extract the full type (everything before the name or default value)
            TSNode firstChild = ts_node_child(child, 0);
            TSNode lastTypeChild = firstChild;
            for (uint32_t j = 0; j < ts_node_child_count(child); j++)
            {
                TSNode paramChild = ts_node_child(child, j);
                if (!ts_node_is_null(nameNode) && ts_node_eq(paramChild, nameNode)) break;
                if (std::string_view(ts_node_type(paramChild)) == "=") break;
                lastTypeChild = paramChild;
            }
            
            if (!ts_node_is_null(firstChild) && !ts_node_is_null(lastTypeChild))
            {
                uint32_t startByte = ts_node_start_byte(firstChild);
                uint32_t endByte = ts_node_end_byte(lastTypeChild);
                if (endByte > startByte)
                {
                    param.typeName = doc.GetText().substr(startByte, endByte - startByte);
                    // Trim trailing spaces
                    while (!param.typeName.empty() && param.typeName.back() == ' ')
                    {
                        param.typeName.pop_back();
                    }
                }
            }


            sym.params.push_back(param);
            
            if (table && !param.name.empty())
            {
                auto paramSym = std::make_shared<Symbol>();
                paramSym->uri = doc.GetUri();
                paramSym->kind       = SymbolKind::Parameter;
                paramSym->name       = param.name;
                paramSym->typeInfo   = param.typeName;
                paramSym->signature  = param.typeName + " " + param.name;
                paramSym->parent     = parentFunc;
                paramSym->selectionRange = SymbolCollector::GetRange(nameNode, doc);
                
                // fullRange is the entire function so FindLocalByNameAt filters correctly by cursor position
                if (parentFunc)
                {
                    paramSym->fullRange = parentFunc->fullRange;
                }
                else
                {
                    paramSym->fullRange = SymbolCollector::GetRange(child, doc);
                }
                
                table->AddLocal(paramSym);
            }
        }
        
        // Build signature
        std::stringstream ss;
        if (!sym.typeInfo.empty()) ss << sym.typeInfo << " ";
        ss << sym.name << "(";
        for (size_t i = 0; i < sym.params.size(); i++)
        {
            ss << sym.params[i].typeName;
            if (!sym.params[i].name.empty())
            {
                ss << " " << sym.params[i].name;
            }
            if (i < sym.params.size() - 1) ss << ", ";
        }
        ss << ")";
        if (sym.isConstMethod)
        {
            ss << " const";
        }
        sym.signature = ss.str();
    }

    void SymbolCollector::RegisterParamsAsLocals(TSNode paramListNode, const Document& doc, SymbolTable& table)
    {
        if (ts_node_is_null(paramListNode)) return;
        
        for (uint32_t i = 0; i < ts_node_child_count(paramListNode); i++)
        {
            TSNode child = ts_node_child(paramListNode, i);
            if (std::string_view(ts_node_type(child)) != "parameter") continue;
            
            TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
            if (ts_node_is_null(nameNode)) continue;
            
            std::string name = GetNodeText(nameNode, doc);
            if (name.empty()) continue;
            
            std::string typeName;
            TSNode firstChild = ts_node_child(child, 0);
            TSNode lastTypeChild = firstChild;
            for (uint32_t j = 0; j < ts_node_child_count(child); j++)
            {
                TSNode paramChild = ts_node_child(child, j);
                if (ts_node_eq(paramChild, nameNode)) break;
                if (std::string_view(ts_node_type(paramChild)) == "=") break;
                lastTypeChild = paramChild;
            }
            
            if (!ts_node_is_null(firstChild) && !ts_node_is_null(lastTypeChild))
            {
                uint32_t startByte = ts_node_start_byte(firstChild);
                uint32_t endByte = ts_node_end_byte(lastTypeChild);
                if (endByte > startByte)
                {
                    typeName = doc.GetText().substr(startByte, endByte - startByte);
                    while (!typeName.empty() && typeName.back() == ' ')
                    {
                        typeName.pop_back();
                    }
                }
            }


            auto paramSym = std::make_shared<Symbol>();
                paramSym->uri = doc.GetUri();
            paramSym->kind       = SymbolKind::Parameter;
            paramSym->name       = name;
            paramSym->typeInfo   = typeName;
            paramSym->selectionRange = GetRange(nameNode, doc);
            
            // Set fullRange to the function block, so FindLocalByNameAt works
            TSNode parentFunc = ts_node_parent(paramListNode);
            if (!ts_node_is_null(parentFunc))
            {
                paramSym->fullRange = GetRange(parentFunc, doc);
            }
            else
            {
                paramSym->fullRange = GetRange(child, doc);
            }
            
            table.AddLocal(paramSym);
        }
    }

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

        if (type == "typedef_declaration")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Typedef;
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode baseTypeNode = ts_node_child_by_field_name(node, "base_type", 9);
            if (!ts_node_is_null(baseTypeNode))
            {
                sym->typeInfo = GetNodeText(baseTypeNode, doc);
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
        else if (type == "func_declaration")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Function;
            sym->fullRange = GetRange(node, doc);
            
            bool isDestructor = false;
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (ts_node_is_null(nameNode))
            {
                // If it's a destructor, the name might not be neatly under a 'name' field, or we might need to find the identifier.
                // Let's just find the identifier child and check if there's a ~ before it.
                for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                {
                    TSNode child = ts_node_child(node, i);
                    if (std::string_view(ts_node_type(child)) == "identifier")
                    {
                        nameNode = child;
                        break;
                    }
                }
            }

            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
                
                TSNode prev = ts_node_prev_sibling(nameNode);
                if (!ts_node_is_null(prev) && std::string_view(ts_node_type(prev)) == "~")
                {
                    isDestructor = true;
                    sym->name = "~" + sym->name;
                    sym->selectionRange = GetRange(prev, doc); // maybe include ~ in selection
                }
            }

            TSNode returnTypeNode = ts_node_child_by_field_name(node, "return_type", 11);
            if (!ts_node_is_null(returnTypeNode))
            {
                sym->typeInfo = GetNodeText(returnTypeNode, doc);
                
                if (parentScope && (parentScope->kind == SymbolKind::Class || parentScope->kind == SymbolKind::Mixin))
                {
                    sym->kind = SymbolKind::Method;
                }
            }
            else if (parentScope && (parentScope->kind == SymbolKind::Class || parentScope->kind == SymbolKind::Mixin))
            {
                sym->kind = isDestructor ? SymbolKind::Destructor : SymbolKind::Constructor;
            }

            TSNode parametersNode = ts_node_child_by_field_name(node, "parameters", 10);
            
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "const")
                {
                    sym->isConstMethod = true;
                    break;
                }
            }
            
            ReadParams(parametersNode, doc, *sym, &table, sym.get());
            
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
        else if (type == "interface_method")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Method;
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode returnTypeNode = ts_node_child_by_field_name(node, "return_type", 11);
            if (!ts_node_is_null(returnTypeNode))
            {
                sym->typeInfo = GetNodeText(returnTypeNode, doc);
            }

            TSNode parametersNode = ts_node_child_by_field_name(node, "parameters", 10);
            
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "const")
                {
                    sym->isConstMethod = true;
                    break;
                }
            }
            
            ReadParams(parametersNode, doc, *sym, &table, sym.get());
            
            if (parentScope)
            {
                sym->parent = parentScope;
                parentScope->children.push_back(sym);
            }
            return;
        }
        else if (type == "variable_declaration")
        {
            TSNode varTypeNode = ts_node_child_by_field_name(node, "var_type", 8);
            std::string typeInfo;
            if (!ts_node_is_null(varTypeNode))
            {
                typeInfo = GetNodeText(varTypeNode, doc);
            }
            else
            {
                for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                {
                    TSNode child = ts_node_child(node, i);
                    if (std::string_view(ts_node_type(child)) == "type")
                    {
                        typeInfo = GetNodeText(child, doc);
                        break;
                    }
                }
            }


            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "variable_declarator")
                {
                    bool hasParamList = false;
                    TSNode paramListNode;
                    for (uint32_t k = 0; k < ts_node_child_count(child); k++)
                    {
                        TSNode grandchild = ts_node_child(child, k);
                        std::string_view gtype = ts_node_type(grandchild);
                        if (gtype == "parameter_list" || gtype == "parameter_list_decl")
                        {
                            hasParamList = true;
                            paramListNode = grandchild;
                            break;
                        }
                    }

                    auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
                    sym->kind = hasParamList ? SymbolKind::Function : SymbolKind::Variable;
                    sym->fullRange = GetRange(node, doc);
                    sym->typeInfo = typeInfo;

                    if (typeInfo == "auto")
                    {
                        uint32_t ccount = ts_node_child_count(child);
                        for (uint32_t c = 0; c < ccount; c++)
                        {
                            TSNode valNode = ts_node_child(child, c);
                            std::string_view valType = ts_node_type(valNode);
                            if (valType == "identifier" && ts_node_is_named(valNode) && ts_node_child_by_field_name(child, "name", 4).id != valNode.id)
                            {
                                std::string varName = GetNodeText(valNode, doc);
                                if (const Symbol* s = table.FindGlobalByName(varName))
                                {
                                    sym->typeInfo = s->typeInfo;
                                }
                                break;
                            }
                            else if (valType == "anonymous_function")
                            {
                                sym->typeInfo = "function";
                                break;
                            }
                        }
                    }

                    TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                    if (!ts_node_is_null(nameNode))
                    {
                        sym->name = GetNodeText(nameNode, doc);
                        sym->selectionRange = GetRange(nameNode, doc);
                    }

                    if (hasParamList)
                    {
                        ReadParams(paramListNode, doc, *sym, parentScope ? nullptr : &table, sym.get());
                    }
                    else
                    {
                        sym->signature = sym->typeInfo + " " + sym->name;
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
                }
            }
            return;
        }
        else if (type == "class_declaration" || type == "interface_declaration" || type == "mixin_declaration")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            if (type == "class_declaration") sym->kind = SymbolKind::Class;
            else if (type == "interface_declaration") sym->kind = SymbolKind::Interface;
            else sym->kind = SymbolKind::Mixin;
            
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                std::string rawName = GetNodeText(nameNode, doc);
                size_t templatePos = rawName.find('<');
                if (templatePos != std::string::npos)
                {
                    sym->name = rawName.substr(0, templatePos);
                }
                else
                {
                    sym->name = rawName;
                }
                sym->selectionRange = GetRange(nameNode, doc);
            }
            
            // Extract base classes, mixins, and modifiers
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view childType = ts_node_type(child);
                
                if (childType == "declaration_modifier")
                {
                    std::string modStr = GetNodeText(child, doc);
                    if (modStr == "shared") sym->isShared = true;
                    else if (modStr == "abstract") sym->isAbstract = true;
                }
                else if (childType == "base_class_list")
                {
                    for (uint32_t j = 0; j < ts_node_child_count(child); j++)
                    {
                        TSNode baseChild = ts_node_child(child, j);
                        if (std::string_view(ts_node_type(baseChild)) == "identifier")
                        {
                            sym->baseClasses.push_back(GetNodeText(baseChild, doc));
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

            TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
            if (!ts_node_is_null(bodyNode))
            {
                uint32_t count = ts_node_child_count(bodyNode);
                for (uint32_t i = 0; i < count; i++)
                {
                    TraverseGlobals(ts_node_child(bodyNode, i), doc, table, sym.get());
                }
            }
            return;
        }
        else if (type == "using_declaration")
        {
            TSNode nsNode = ts_node_child_by_field_name(node, "namespace", 9); // usually it's just 'namespace' or we can find scoped_identifier
            // Let's just find the scoped_identifier or identifier
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                std::string_view childType = ts_node_type(child);
                if (childType == "scoped_identifier" || childType == "identifier")
                {
                    table.AddUsingNamespace(GetNodeText(child, doc));
                    break;
                }
            }
            return;
        }
        else if (type == "namespace_declaration")
        {
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            std::string fullName = GetNodeText(nameNode, doc);
            
            std::vector<std::string> parts;
            size_t start = 0;
            size_t end = fullName.find("::");
            while (end != std::string::npos)
            {
                parts.push_back(fullName.substr(start, end - start));
                start = end + 2;
                end = fullName.find("::", start);
            }
            parts.push_back(fullName.substr(start));

            Symbol* currentParent = parentScope;

            for (const std::string& part : parts)
            {
                std::shared_ptr<Symbol> sym;
                bool found = false;

                if (currentParent)
                {
                    for (auto& child : currentParent->children)
                    {
                        if (child->name == part && child->kind == SymbolKind::Namespace)
                        {
                            sym = child;
                            found = true;
                            break;
                        }
                    }
                }
                else
                {
                    Symbol* existing = table.FindGlobalByName(part);
                    if (existing && existing->kind == SymbolKind::Namespace)
                    {
                        auto it = table.GetGlobals().find(part);
                        if (it != table.GetGlobals().end() && !it->second.empty())
                        {
                            // Si hay múltiples, tomamos el primero para el using (comportamiento legacy/fallback)
                            const Symbol* nsSym = it->second.front().get();
                            if (nsSym->kind == SymbolKind::Namespace)
                            {
                                sym = it->second.front();
                                found = true;
                            }
                        }
                    }
                }

                if (!found)
                {
                    sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
                    sym->kind = SymbolKind::Namespace;
                    sym->name = part;
                    sym->fullRange = GetRange(node, doc);
                    sym->selectionRange = GetRange(nameNode, doc); // Approximate for intermediate parts
                    
                    if (currentParent)
                    {
                        sym->parent = currentParent;
                        currentParent->children.push_back(sym);
                    }
                    else
                    {
                        table.AddGlobal(sym);
                    }
                }
                
                currentParent = sym.get();
            }

            TSNode bodyNode = ts_node_child_by_field_name(node, "body", 4);
            if (!ts_node_is_null(bodyNode) && currentParent)
            {
                uint32_t count = ts_node_child_count(bodyNode);
                for (uint32_t i = 0; i < count; i++)
                {
                    TraverseGlobals(ts_node_child(bodyNode, i), doc, table, currentParent);
                }
            }
            return;
        }
        else if (type == "enum_declaration")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Enum;
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "enum_member")
                {
                    auto memberSym = std::make_shared<Symbol>();
                    memberSym->kind = SymbolKind::EnumMember;
                    memberSym->parent = sym.get();
                    memberSym->fullRange = GetRange(child, doc);
                    
                    TSNode mNameNode = ts_node_child_by_field_name(child, "name", 4);
                    if (!ts_node_is_null(mNameNode))
                    {
                        memberSym->name = GetNodeText(mNameNode, doc);
                        memberSym->selectionRange = GetRange(mNameNode, doc);
                    }
                    sym->children.push_back(memberSym);
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
        else if (type == "funcdef_declaration")
        {
            auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Funcdef;
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode returnTypeNode = ts_node_child_by_field_name(node, "return_type", 11);
            if (!ts_node_is_null(returnTypeNode))
            {
                sym->typeInfo = GetNodeText(returnTypeNode, doc);
            }

            TSNode parametersNode = ts_node_child_by_field_name(node, "parameters", 10);
            ReadParams(parametersNode, doc, *sym, &table, sym.get());
            
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
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Property;
            sym->fullRange = GetRange(node, doc);
            
            TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode typeNode = ts_node_child_by_field_name(node, "prop_type", 9);
            if (!ts_node_is_null(typeNode))
            {
                sym->typeInfo = GetNodeText(typeNode, doc);
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

            TSNode varTypeNode = ts_node_child_by_field_name(node, "var_type", 8);
            std::string typeInfo;
            if (!ts_node_is_null(varTypeNode))
            {
                typeInfo = GetNodeText(varTypeNode, doc);
            }
            else
            {
                for (uint32_t i = 0; i < ts_node_child_count(node); i++)
                {
                    TSNode child = ts_node_child(node, i);
                    if (std::string_view(ts_node_type(child)) == "type")
                    {
                        typeInfo = GetNodeText(child, doc);
                        break;
                    }
                }
            }
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "variable_declarator")
                    {
                        auto sym = std::make_shared<Symbol>();
                    sym->uri = doc.GetUri();
            sym->uri = doc.GetUri();
                        sym->kind = SymbolKind::Variable;
                        
                        // Set fullRange to the enclosing block so FindLocalByNameAt works for the rest of the block
                        TSNode blockNode = ts_node_parent(node);
                        while (!ts_node_is_null(blockNode) && std::string_view(ts_node_type(blockNode)) != "statement_block")
                        {
                            blockNode = ts_node_parent(blockNode);
                        }
                        if (!ts_node_is_null(blockNode))
                        {
                            sym->fullRange = GetRange(blockNode, doc);
                        }
                        else
                        {
                            sym->fullRange = GetRange(node, doc);
                        }
                        
                        sym->typeInfo = typeInfo;

                        if (typeInfo == "auto")
                        {
                            uint32_t ccount = ts_node_child_count(child);
                            for (uint32_t c = 0; c < ccount; c++)
                            {
                                TSNode valNode = ts_node_child(child, c);
                                std::string_view valType = ts_node_type(valNode);
                                if (valType == "identifier" && ts_node_is_named(valNode) && ts_node_child_by_field_name(child, "name", 4).id != valNode.id)
                                {
                                    std::string varName = GetNodeText(valNode, doc);
                                    if (const Symbol* s = table.FindLocalByName(varName))
                                    {
                                        sym->typeInfo = s->typeInfo;
                                    }
                                    else if (const Symbol* s = table.FindGlobalByName(varName))
                                    {
                                        sym->typeInfo = s->typeInfo;
                                    }
                                    break;
                                }
                                else if (valType == "anonymous_function")
                                {
                                    sym->typeInfo = "function";
                                    break;
                                }
                            }
                        }

                        TSNode nameNode = ts_node_child_by_field_name(child, "name", 4);
                        if (!ts_node_is_null(nameNode))
                        {
                            sym->name = GetNodeText(nameNode, doc);
                            sym->selectionRange = GetRange(nameNode, doc);
                        }

                        if (currentScope)
                        {
                            sym->parent = currentScope;
                        }
                        table.AddLocal(sym);
                    }
                }
            
            // Note: We still fall through to traverse the children of this node, 
            // because even if it's a misparsed call, its arguments might contain local variables!
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_child(node, i);
            TraverseLocals(child, doc, table, currentScope);
        }
    }
}
