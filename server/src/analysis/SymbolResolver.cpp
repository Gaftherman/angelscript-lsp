#include "analysis/SymbolResolver.h"
#include <string_view>

namespace analysis
{
    static const Symbol* FindNamespace(const SymbolTable& table, const std::string& path) {
        std::vector<std::string> parts;
        size_t start = 0;
        size_t end = path.find("::");
        while (end != std::string::npos) {
            parts.push_back(path.substr(start, end - start));
            start = end + 2;
            end = path.find("::", start);
        }
        parts.push_back(path.substr(start));

        const Symbol* current = nullptr;
        for (const std::string& part : parts) {
            if (!current) {
                current = table.FindGlobalByName(part);
            } else {
                const Symbol* next = nullptr;
                for (const auto& child : current->children) {
                    if (child->name == part && child->kind == SymbolKind::Namespace) {
                        next = child.get();
                        break;
                    }
                }
                current = next;
            }
            if (!current) break;
        }
        return current;
    }

    const Symbol* SymbolResolver::ResolveAt(const Document& doc, const SymbolTable& table, uint32_t line, uint32_t character)
    {
        TSNode node = doc.NodeAt(line, character);
        if (ts_node_is_null(node)) return nullptr;

        // Climb up to identifier if we are on one, or find nearest identifier
        while (!ts_node_is_null(node))
        {
            std::string_view type = ts_node_type(node);
            if (type == "identifier" || type == "type_identifier") break;
            node = ts_node_parent(node);
        }

        if (ts_node_is_null(node)) return nullptr;

        // Extract identifier text
        std::string_view identSv = doc.SourceAt(node);
        std::string identText(identSv.begin(), identSv.end());

        TSNode parent = ts_node_parent(node);
        std::string_view parentType = ts_node_is_null(parent) ? "" : ts_node_type(parent);

        // Member resolution: myObject.Prop
        if (parentType == "member_expression")
        {
            TSNode memberNode = ts_node_child_by_field_name(parent, "member", 6);
            TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
            
            if (!ts_node_is_null(memberNode) && ts_node_eq(node, memberNode))
            {
                std::string_view objSv = doc.SourceAt(objectNode);
                std::string objText(objSv.begin(), objSv.end());
                    
                // 1. Find the object's symbol to get its type
                const Symbol* objSym = table.FindLocalByName(objText);
                if (!objSym)
                    objSym = table.FindGlobalByName(objText);
                        
                if (objSym)
                {
                        // 2. Clean the type name ("Enemy@" -> "Enemy")
                        std::string typeName = CleanTypeName(objSym->typeInfo);
                        
                        size_t colonPos = typeName.rfind("::");
                        if (colonPos != std::string::npos) {
                            typeName = typeName.substr(colonPos + 2);
                        }
                        
                        // 3. Find the type (class) symbol
                        const Symbol* classSym = table.FindByNameDeep(typeName);
                        if (classSym)
                        {
                            // 4. Find the member inside the class or its base classes
                            auto findMember = [&](auto& self, const Symbol* cSym) -> const Symbol* {
                                if (!cSym) return nullptr;
                                for (const auto& child : cSym->children) {
                                    if (child->name == identText) return child.get();
                                }
                                for (const auto& baseName : cSym->baseClasses) {
                                    const Symbol* baseSym = table.FindByNameDeep(baseName);
                                    if (baseSym) {
                                        if (const Symbol* found = self(self, baseSym)) return found;
                                    }
                                }
                                return nullptr;
                            };
                            
                            if (const Symbol* found = findMember(findMember, classSym)) {
                                return found;
                            }
                        }
                    }
                    return nullptr; // Failed to resolve member
                }
        }

        // Namespace resolution (Valid scoped_identifier)
        if (parentType == "scoped_identifier")
        {
            uint32_t count = ts_node_child_count(parent);
            if (count >= 3)
            {
                TSNode lastId = ts_node_child(parent, count - 1);
                if (ts_node_eq(node, lastId))
                {
                    TSNode nsNode = ts_node_child(parent, 0);
                    std::string_view nsSv = doc.SourceAt(nsNode);
                    std::string nsText(nsSv.begin(), nsSv.end());
                    
                    const Symbol* nsSym = FindNamespace(table, nsText);
                    if (nsSym && nsSym->kind == SymbolKind::Namespace)
                    {
                        for (const auto& child : nsSym->children)
                        {
                            if (child->name == identText) return child.get();
                        }
                    }
                    return nullptr;
                }
            }
        }

        // Namespace resolution (Tree-sitter ERROR workaround for expressions like Math::PI)
        TSNode prevSibling = ts_node_prev_sibling(node);
        if (!ts_node_is_null(prevSibling) && std::string_view(ts_node_type(prevSibling)) == "ERROR")
        {
            uint32_t errCount = ts_node_child_count(prevSibling);
            if (errCount >= 2)
            {
                TSNode colonNode = ts_node_child(prevSibling, errCount - 1);
                if (std::string_view(ts_node_type(colonNode)) == "::")
                {
                    TSNode nsNode = ts_node_child(prevSibling, 0);
                    std::string_view nsSv = doc.SourceAt(nsNode);
                    std::string nsText(nsSv.begin(), nsSv.end());
                    
                    const Symbol* nsSym = FindNamespace(table, nsText);
                    if (nsSym && nsSym->kind == SymbolKind::Namespace)
                    {
                        for (const auto& child : nsSym->children)
                        {
                            if (child->name == identText) return child.get();
                        }
                    }
                    // Fallthrough
                }
            }
        }
        
        // Namespace resolution workaround for Combat::Fire() which produces `ERROR -> type -> scope + datatype -> identifier`
        if (parentType == "datatype")
        {
            TSNode datatypePrevSibling = ts_node_prev_sibling(parent);
            if (!ts_node_is_null(datatypePrevSibling) && std::string_view(ts_node_type(datatypePrevSibling)) == "scope")
            {
                // Child 0 of scope should be the namespace identifier
                TSNode nsNode = ts_node_child(datatypePrevSibling, 0);
                std::string_view nsSv = doc.SourceAt(nsNode);
                std::string nsText(nsSv.begin(), nsSv.end());
                
                const Symbol* nsSym = FindNamespace(table, nsText);
                if (nsSym && nsSym->kind == SymbolKind::Namespace)
                {
                    for (const auto& child : nsSym->children)
                    {
                        if (child->name == identText) return child.get();
                    }
                }
                return nullptr;
            }
        }

        // Namespace resolution workaround for `Engine::Math::Lerp()` which parses as variable_declaration with an ERROR node for `::`
        if (parentType == "variable_declarator")
        {
            TSNode varDecl = ts_node_parent(parent);
            if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
            {
                // Find the `type` node of the variable declaration
                TSNode tNode = ts_node_child_by_field_name(varDecl, "var_type", 8);
                if (ts_node_is_null(tNode)) {
                    for (uint32_t j = 0; j < ts_node_child_count(varDecl); j++) {
                        TSNode child = ts_node_child(varDecl, j);
                        if (std::string_view(ts_node_type(child)) == "type") {
                            tNode = child;
                            break;
                        }
                    }
                }
                
                if (!ts_node_is_null(tNode))
                {
                    std::string_view nsSv = doc.SourceAt(tNode);
                    std::string nsText(nsSv.begin(), nsSv.end());
                    
                    const Symbol* nsSym = FindNamespace(table, nsText);
                    if (nsSym && nsSym->kind == SymbolKind::Namespace)
                    {
                        for (const auto& child : nsSym->children)
                        {
                            if (child->name == identText) return child.get();
                        }
                    }
                    // What if nsSym is an Enum? Let's check that too for Engine::Math::Lerp vs State::IDLE?
                    // No, `State s = IDLE` has type `State`. nsSym is `State` (Enum).
                    // Wait, if it IS an Enum, and the child matches, we could return it!
                    if (nsSym && nsSym->kind == SymbolKind::Enum)
                    {
                        for (const auto& child : nsSym->children)
                        {
                            if (child->name == identText) return child.get();
                        }
                    }
                    // Fallthrough
                }
            }
        }

        // Local resolution
        if (const Symbol* localSym = table.FindLocalByName(identText))
        {
            return localSym;
        }

        // Check in using namespaces
        for (const std::string& usingNs : table.GetUsingNamespaces())
        {
            const Symbol* nsSym = FindNamespace(table, usingNs);
            if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                for (const auto& child : nsSym->children) {
                    if (child->name == identText) return child.get();
                }
            }
        }

        // Fallback to global search (deep)
        if (const Symbol* globalSym = table.FindByNameDeep(identText))
        {
            return globalSym;
        }

        // Deep search for Enum members (AngelScript allows un-prefixed enum members)
        for (const auto& [name, sym] : table.GetGlobals())
        {
            if (sym->kind == SymbolKind::Enum)
            {
                for (const auto& child : sym->children)
                {
                    if (child->name == identText)
                    {
                        return child.get();
                    }
                }
            }
        }
        
        // Scope-aware Deep member search
        std::string containingClass;
        TSNode climb = ts_node_parent(node);
        while (!ts_node_is_null(climb)) {
            std::string_view ct = ts_node_type(climb);
            if (ct == "class_declaration") {
                TSNode nameNode = ts_node_child_by_field_name(climb, "name", 4);
                if (!ts_node_is_null(nameNode)) {
                    std::string_view sv = doc.SourceAt(nameNode);
                    containingClass = std::string(sv.begin(), sv.end());
                }
                break;
            }
            climb = ts_node_parent(climb);
        }

        if (!containingClass.empty()) {
            const Symbol* classSym = table.FindByNameDeep(containingClass);
            if (classSym) {
                if (identText == containingClass) {
                    return classSym;
                }
                for (const auto& child : classSym->children) {
                    if (child->name == identText) return child.get();
                }
            }
            // Do not fall back to other classes if we are inside a specific class
            return nullptr;
        }

        // Generic Deep member search (fallback for when not inside a class, e.g. for globals or namespaces)
        for (const auto& [name, sym] : table.GetGlobals()) {
            if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Namespace) {
                for (const auto& child : sym->children) {
                    if (child->name == identText) return child.get();
                    // Buscar también en hijos de hijos (métodos dentro de clases)
                    for (const auto& grandchild : child->children) {
                        if (grandchild->name == identText) return grandchild.get();
                    }
                }
            }
        }

        return nullptr;
    }

    std::string SymbolResolver::CleanTypeName(std::string_view raw)
    {
        std::string result(raw);
        
        // Remove keywords from the beginning
        auto stripPrefix = [&result](const std::string& prefix) {
            if (result.starts_with(prefix))
            {
                result = result.substr(prefix.length());
                // Strip spaces after prefix
                while (!result.empty() && result[0] == ' ')
                    result = result.substr(1);
            }
        };

        stripPrefix("const ");
        stripPrefix("inout ");
        stripPrefix("in ");
        stripPrefix("out ");
        
        // Remove symbols anywhere
        auto removeChar = [&result](char c) {
            size_t pos;
            while ((pos = result.find(c)) != std::string::npos)
                result.erase(pos, 1);
        };
        
        removeChar('@');
        removeChar('&');
        
        // Remove array brackets
        size_t arrPos;
        while ((arrPos = result.find("[]")) != std::string::npos)
            result.erase(arrPos, 2);
            
        // Strip trailing spaces
        while (!result.empty() && result.back() == ' ')
            result.pop_back();

        return result;
    }
}
