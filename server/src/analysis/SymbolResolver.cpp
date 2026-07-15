#include "analysis/SymbolResolver.h"
#include <string_view>

namespace analysis
{
    const Symbol* SymbolResolver::ResolveAt(const Document& doc, const SymbolTable& table, uint32_t line, uint32_t character)
    {
        TSNode node = doc.NodeAt(line, character);
        if (ts_node_is_null(node)) return nullptr;

        // Climb up to identifier if we are on one, or find nearest identifier
        while (!ts_node_is_null(node))
        {
            std::string_view type = ts_node_type(node);
            if (type == "identifier") break;
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
            // Usually: [object] [.] [member]
            // We need to check if the node we are on is the member (right side)
            uint32_t count = ts_node_child_count(parent);
            if (count >= 3)
            {
                TSNode memberNode = ts_node_child(parent, count - 1); // the last child is the member
                if (ts_node_eq(node, memberNode))
                {
                    TSNode objectNode = ts_node_child(parent, 0);
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
                        
                        // 3. Find the type (class) symbol
                        const Symbol* classSym = table.FindGlobalByName(typeName);
                        if (classSym)
                        {
                            // 4. Find the member inside the class
                            for (const auto& child : classSym->children)
                            {
                                if (child->name == identText)
                                {
                                    return child.get();
                                }
                            }
                        }
                    }
                    return nullptr; // Failed to resolve member
                }
            }
        }

        // Namespace resolution (Valid scoped_identifier)
        if (parentType == "scoped_identifier")
        {
            uint32_t count = ts_node_child_count(parent);
            TSNode lastId = ts_node_child(parent, count - 1);
            if (ts_node_eq(node, lastId))
            {
                TSNode nsNode = ts_node_child(parent, 0);
                std::string_view nsSv = doc.SourceAt(nsNode);
                std::string nsText(nsSv.begin(), nsSv.end());
                
                const Symbol* nsSym = table.FindGlobalByName(nsText);
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
                    
                    const Symbol* nsSym = table.FindGlobalByName(nsText);
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
                
                const Symbol* nsSym = table.FindGlobalByName(nsText);
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

        // Local resolution
        if (const Symbol* localSym = table.FindLocalByName(identText))
        {
            return localSym;
        }

        // Fallback to global search
        return table.FindGlobalByName(identText);
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
