#include "analysis/SymbolCollector.h"
#include "analysis/SymbolResolver.h"
#include <string_view>
#include <sstream>
#include <iostream>

static inline TSNode FieldChild(TSNode node, const char* field)
{
    return ts_node_child_by_field_name(node, field, (uint32_t)strlen(field));
}

namespace analysis
{
    std::string SymbolCollector::GetNodeText(TSNode node, const Document &doc)
    {
        if (ts_node_is_null(node))
            return "";
        std::string_view sv = doc.SourceAt(node);
        return std::string(sv.begin(), sv.end());
    }

    lsp::Range SymbolCollector::GetRange(TSNode node, const Document &doc)
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

    static void ReadParams(TSNode paramListNode, const Document &doc, Symbol &sym, SymbolTable *table = nullptr, Symbol *parentFunc = nullptr)
    {
        if (ts_node_is_null(paramListNode))
            return;

        for (uint32_t i = 0; i < ts_node_child_count(paramListNode); i++)
        {
            TSNode child = ts_node_child(paramListNode, i);
            if (std::string_view(ts_node_type(child)) != "parameter")
                continue;

            TSNode nameNode = FieldChild(child, "name");

            SymbolParam param;
            if (!ts_node_is_null(nameNode))
                param.name = SymbolCollector::GetNodeText(nameNode, doc);

            // Extract the full type (everything before the name or default value)
            TSNode firstChild = ts_node_child(child, 0);
            TSNode lastTypeChild = firstChild;
            for (uint32_t j = 0; j < ts_node_child_count(child); j++)
            {
                TSNode paramChild = ts_node_child(child, j);
                if (!ts_node_is_null(nameNode) && ts_node_eq(paramChild, nameNode))
                    break;
                if (std::string_view(ts_node_type(paramChild)) == "=")
                    break;
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
            // Extract default value
            for (uint32_t j = 0; j < ts_node_child_count(child); j++)
            {
                TSNode paramChild = ts_node_child(child, j);
                if (std::string_view(ts_node_type(paramChild)) == "=")
                {
                    uint32_t startByte = ts_node_end_byte(paramChild);
                    uint32_t endByte = ts_node_end_byte(child);
                    if (endByte > startByte)
                    {
                        std::string defVal = doc.GetText().substr(startByte, endByte - startByte);
                        size_t first = defVal.find_first_not_of(" \t\r\n");
                        if (first != std::string::npos)
                        {
                            size_t last = defVal.find_last_not_of(" \t\r\n");
                            param.defaultValue = defVal.substr(first, (last - first + 1));
                        }
                    }
                    break;
                }
            }

            sym.params.push_back(param);

            if (table && !param.name.empty())
            {
                auto paramSym = std::make_shared<Symbol>();
                paramSym->uri = doc.GetUri();
                paramSym->kind = SymbolKind::Parameter;
                paramSym->name = param.name;
                paramSym->typeInfo = param.typeName;
                paramSym->parent = parentFunc;
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
    }
    static std::string ExtractDocComments(TSNode declNode, const Document &doc)
    {
        std::vector<std::string> lines;
        TSNode current = ts_node_prev_sibling(declNode);
        uint32_t lastStartRow = ts_node_start_point(declNode).row;

        while (!ts_node_is_null(current))
        {
            std::string_view type = ts_node_type(current);
            if (type == "comment")
            {
                uint32_t endRow = ts_node_end_point(current).row;
                if (lastStartRow > endRow + 1)
                {
                    break;
                }
                std::string text = SymbolCollector::GetNodeText(current, doc);
                lines.insert(lines.begin(), text);
                lastStartRow = ts_node_start_point(current).row;
            }
            else if (!ts_node_is_named(current))
            {
                // Skip anonymous nodes (like punctuation: comma, etc)
            }
            else
            {
                break;
            }
            current = ts_node_prev_sibling(current);
        }

        if (lines.empty())
            return "";

        std::string cleanDocs;
        for (const std::string &line : lines)
        {
            std::istringstream iss(line);
            std::string subline;
            while (std::getline(iss, subline))
            {
                size_t start = subline.find_first_not_of(" \t");
                if (start != std::string::npos)
                    subline = subline.substr(start);
                else
                    subline = "";

                size_t endComment = subline.find("*/");
                if (endComment != std::string::npos)
                    subline = subline.substr(0, endComment);

                if (subline.starts_with("///"))
                    subline = subline.substr(3);
                else if (subline.starts_with("//"))
                    subline = subline.substr(2);
                else if (subline.starts_with("/**"))
                    subline = subline.substr(3);
                else if (subline.starts_with("/*"))
                    subline = subline.substr(2);
                else if (subline.starts_with("*"))
                    subline = subline.substr(1);

                start = subline.find_first_not_of(" \t");
                if (start != std::string::npos)
                    subline = subline.substr(start);
                else
                    subline = "";

                size_t end = subline.find_last_not_of(" \t\r");
                if (end != std::string::npos)
                    subline = subline.substr(0, end + 1);

                if (!cleanDocs.empty())
                    cleanDocs += "\n";
                cleanDocs += subline;
            }
        }

        size_t firstNonNewline = cleanDocs.find_first_not_of("\n\r \t");
        if (firstNonNewline != std::string::npos)
            cleanDocs = cleanDocs.substr(firstNonNewline);
        else
            return "";

        size_t lastNonNewline = cleanDocs.find_last_not_of("\n\r \t");
        if (lastNonNewline != std::string::npos)
            cleanDocs = cleanDocs.substr(0, lastNonNewline + 1);

        return cleanDocs;
    }

    void SymbolCollector::RegisterParamsAsLocals(TSNode paramListNode, const Document &doc, SymbolTable &table)
    {
        Symbol dummy;
        ReadParams(paramListNode, doc, dummy, &table, nullptr);
    }

    void SymbolCollector::CollectGlobals(const Document &doc, SymbolTable &table)
    {
        TSNode root = doc.RootNode();
        if (ts_node_is_null(root))
            return;
        TraverseGlobals(root, doc, table, nullptr);
    }

    static void ExtractModifiers(TSNode node, const Document &doc, Symbol &sym)
    {
        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_child(node, i);
            std::string_view type = ts_node_type(child);
            if (type == "func_attributes")
            {
                ExtractModifiers(child, doc, sym);
                continue;
            }
            if (type == "modifier" || type == "declaration_modifier" || type == "private" || type == "protected" || type == "const" || type == "final" || type == "override" || type == "abstract" || type == "shared")
            {
                std::string modText = SymbolCollector::GetNodeText(child, doc);
                if (modText == "private")
                    sym.isPrivate = true;
                else if (modText == "protected")
                    sym.isProtected = true;
                else if (modText == "const")
                    sym.isConstMethod = true;
                else if (modText == "final")
                    sym.isFinal = true;
                else if (modText == "override")
                    sym.isOverride = true;
                else if (modText == "abstract")
                    sym.isAbstract = true;
                else if (modText == "shared")
                    sym.isShared = true;
            }
        }
    }

    
    static void BuildVariableSymbols(TSNode node, const Document &doc, SymbolTable &table, Symbol *parentScope, bool isLocal)
    {
        TSNode varTypeNode = FieldChild(node, "var_type");
        std::string typeInfo;
        if (!ts_node_is_null(varTypeNode))
        {
            typeInfo = SymbolCollector::GetNodeText(varTypeNode, doc);
        }
        else
        {
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "type")
                {
                    typeInfo = SymbolCollector::GetNodeText(child, doc);
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
                sym->kind = hasParamList ? SymbolKind::Function : SymbolKind::Variable;
                
                if (isLocal)
                {
                    TSNode blockNode = ts_node_parent(node);
                    while (!ts_node_is_null(blockNode) && std::string_view(ts_node_type(blockNode)) != "statement_block")
                    {
                        blockNode = ts_node_parent(blockNode);
                    }
                    if (!ts_node_is_null(blockNode))
                        sym->fullRange = SymbolCollector::GetRange(blockNode, doc);
                    else
                        sym->fullRange = SymbolCollector::GetRange(node, doc);
                }
                else
                {
                    sym->fullRange = SymbolCollector::GetRange(node, doc);
                }

                sym->docComment = ExtractDocComments(node, doc);
                sym->typeInfo = typeInfo;
                ExtractModifiers(node, doc, *sym);

                TSNode valNodeForAuto = {0};
                for (uint32_t c = 0; c < ts_node_child_count(child); c++)
                {
                    TSNode vChild = ts_node_child(child, c);
                    if (std::string_view(ts_node_type(vChild)) == "=" && c + 1 < ts_node_child_count(child))
                    {
                        TSNode valNode = ts_node_child(child, c + 1);
                        valNodeForAuto = valNode;
                        sym->value = SymbolCollector::GetNodeText(valNode, doc);
                        break;
                    }
                }

                if (typeInfo == "auto" && !ts_node_is_null(valNodeForAuto))
                {
                    std::string inferred = SymbolResolver::EvaluateExpressionType(doc, table, valNodeForAuto);
                    if (!inferred.empty())
                    {
                        sym->typeInfo = inferred;
                    }
                }

                TSNode nameNode = FieldChild(child, "name");
                if (!ts_node_is_null(nameNode))
                {
                    sym->name = SymbolCollector::GetNodeText(nameNode, doc);
                    sym->selectionRange = SymbolCollector::GetRange(nameNode, doc);
                }

                if (hasParamList)
                {
                    SymbolTable *tablePtr = parentScope ? nullptr : &table;
                    ReadParams(paramListNode, doc, *sym, tablePtr, sym.get());
                }

                if (isLocal)
                {
                    if (parentScope)
                        sym->parent = parentScope;
                    table.AddLocal(sym);
                }
                else
                {
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
        }
    }

    void SymbolCollector::TraverseGlobals(TSNode node, const Document &doc, SymbolTable &table, Symbol *parentScope)
    {
        if (ts_node_is_null(node))
            return;

        std::string_view type = ts_node_type(node);

        if (type == "typedef_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->uri = doc.GetUri();
            sym->kind = SymbolKind::Typedef;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);

            TSNode nameNode = FieldChild(node, "name");
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode baseTypeNode = FieldChild(node, "base_type");
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
            sym->kind = SymbolKind::Function;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);
            ExtractModifiers(node, doc, *sym);

            bool isDestructor = false;
            TSNode nameNode = FieldChild(node, "name");
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

            TSNode returnTypeNode = FieldChild(node, "return_type");
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

            TSNode parametersNode = FieldChild(node, "parameters");

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
            sym->kind = SymbolKind::Method;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);
            ExtractModifiers(node, doc, *sym);

            TSNode nameNode = FieldChild(node, "name");
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode returnTypeNode = FieldChild(node, "return_type");
            if (!ts_node_is_null(returnTypeNode))
            {
                sym->typeInfo = GetNodeText(returnTypeNode, doc);
            }

            TSNode parametersNode = FieldChild(node, "parameters");

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
            BuildVariableSymbols(node, doc, table, parentScope, false);
        }
        else if (type == "class_declaration" || type == "interface_declaration" || type == "mixin_declaration")
        {
            auto sym = std::make_shared<Symbol>();
            sym->uri = doc.GetUri();
            if (type == "class_declaration")
                sym->kind = SymbolKind::Class;
            else if (type == "interface_declaration")
                sym->kind = SymbolKind::Interface;
            else
                sym->kind = SymbolKind::Mixin;

            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);
            ExtractModifiers(node, doc, *sym);

            TSNode nameNode = FieldChild(node, "name");
            if (!ts_node_is_null(nameNode))
            {
                std::string rawName = GetNodeText(nameNode, doc);
                
                // tree-sitter-angelscript might parse `<T>` as an ERROR node after the identifier.
                // We should extract the text from the start of nameNode to the start of the body or the end of the node.
                TSNode bodyNode = FieldChild(node, "body");
                uint32_t startByte = ts_node_start_byte(nameNode);
                uint32_t endByte = ts_node_is_null(bodyNode) ? ts_node_end_byte(node) : ts_node_start_byte(bodyNode);
                
                if (endByte > startByte)
                {
                    std::string fullHeader = doc.GetText().substr(startByte, endByte - startByte);
                    size_t templatePos = fullHeader.find('<');
                    if (templatePos != std::string::npos)
                    {
                        sym->name = fullHeader.substr(0, templatePos);
                        // trim trailing spaces of name
                        sym->name.erase(sym->name.find_last_not_of(" \n\r\t") + 1);
                        size_t endTemplate = fullHeader.find('>', templatePos);
                        if (endTemplate != std::string::npos)
                        {
                            sym->templateParam = fullHeader.substr(templatePos + 1, endTemplate - templatePos - 1);
                        }
                    }
                    else
                    {
                        sym->name = rawName;
                    }
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
                    if (modStr == "shared")
                        sym->isShared = true;
                    else if (modStr == "abstract")
                        sym->isAbstract = true;
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

            TSNode bodyNode = FieldChild(node, "body");
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
            TSNode nsNode = FieldChild(node, "namespace"); // usually it's just 'namespace' or we can find scoped_identifier
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
            TSNode nameNode = FieldChild(node, "name");
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

            Symbol *currentParent = parentScope;

            for (const std::string &part : parts)
            {
                std::shared_ptr<Symbol> sym;
                bool found = false;

                if (currentParent)
                {
                    for (auto &child : currentParent->children)
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
                    Symbol *existing = table.FindGlobalByName(part);
                    if (existing && existing->kind == SymbolKind::Namespace)
                    {
                        auto it = table.GetGlobals().find(part);
                        if (it != table.GetGlobals().end() && !it->second.empty())
                        {
                            // Si hay múltiples, tomamos el primero para el using (comportamiento legacy/fallback)
                            const Symbol *nsSym = it->second.front().get();
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

            if (currentParent)
            {
                currentParent->docComment = ExtractDocComments(node, doc);
            }

            TSNode bodyNode = FieldChild(node, "body");
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
            sym->kind = SymbolKind::Enum;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);

            TSNode nameNode = FieldChild(node, "name");
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
                    memberSym->docComment = ExtractDocComments(child, doc);

                    TSNode mNameNode = FieldChild(child, "name");
                    if (!ts_node_is_null(mNameNode))
                    {
                        memberSym->name = GetNodeText(mNameNode, doc);
                        memberSym->selectionRange = GetRange(mNameNode, doc);
                    }
                    TSNode valueNode = FieldChild(child, "value");
                    if (!ts_node_is_null(valueNode))
                    {
                        memberSym->value = GetNodeText(valueNode, doc);
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
            sym->kind = SymbolKind::Funcdef;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);

            TSNode nameNode = FieldChild(node, "name");
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode returnTypeNode = FieldChild(node, "return_type");
            if (!ts_node_is_null(returnTypeNode))
            {
                sym->typeInfo = GetNodeText(returnTypeNode, doc);
            }

            TSNode parametersNode = FieldChild(node, "parameters");
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
            sym->kind = SymbolKind::Property;
            sym->fullRange = GetRange(node, doc);
            sym->docComment = ExtractDocComments(node, doc);

            TSNode nameNode = FieldChild(node, "name");
            if (!ts_node_is_null(nameNode))
            {
                sym->name = GetNodeText(nameNode, doc);
                sym->selectionRange = GetRange(nameNode, doc);
            }

            TSNode typeNode = FieldChild(node, "prop_type");
            if (!ts_node_is_null(typeNode))
            {
                sym->typeInfo = GetNodeText(typeNode, doc);
            }
            ExtractModifiers(node, doc, *sym);
            std::string accessors = "{ ";
            bool hasAccessors = false;
            for (uint32_t i = 0; i < ts_node_child_count(node); i++)
            {
                TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) == "accessor")
                {
                    hasAccessors = true;
                    bool isGet = false;
                    bool isSet = false;
                    bool isConst = false;
                    for (uint32_t j = 0; j < ts_node_child_count(child); j++)
                    {
                        std::string_view accType = ts_node_type(ts_node_child(child, j));
                        if (accType == "get")
                            isGet = true;
                        if (accType == "set")
                            isSet = true;
                        if (accType == "const")
                            isConst = true;
                    }
                    if (isGet)
                        accessors += "get" + std::string(isConst ? " const" : "") + "; ";
                    if (isSet)
                        accessors += "set; ";
                }
            }
            
            if (hasAccessors)
            {
                accessors += "}";
                sym->accessors = accessors;
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

    void SymbolCollector::TraverseLocals(TSNode node, const Document &doc, SymbolTable &table, Symbol *currentScope)
    {
        if (ts_node_is_null(node))
            return;

        std::string_view type = ts_node_type(node);

        if (type == "variable_declaration")
        {
            BuildVariableSymbols(node, doc, table, currentScope, true);
        }

        uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; i++)
        {
            TSNode child = ts_node_child(node, i);
            TraverseLocals(child, doc, table, currentScope);
        }
    }
}
