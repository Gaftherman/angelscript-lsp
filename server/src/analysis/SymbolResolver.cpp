#include "analysis/SymbolResolver.h"
#include <string_view>
#include <optional>
#include <algorithm>

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

    static std::optional<SymbolKind> InferExpectedKind(TSNode node, TSNode parent, std::string_view parentType)
    {
        if (parentType == "type" || parentType == "datatype") {
            TSNode gp = ts_node_parent(parent);
            if (!ts_node_is_null(gp)) {
                TSNode ggp = ts_node_parent(gp);
                if (!ts_node_is_null(ggp) && std::string_view(ts_node_type(ggp)) == "ERROR") {
                    for (uint32_t i = 0; i < ts_node_child_count(ggp); i++) {
                        if (std::string_view(ts_node_type(ts_node_child(ggp, i))) == "argument_list") {
                            return SymbolKind::Function;
                        }
                    }
                }
            }
            // Un identificador usado como tipo (ej. `MyClass c;`)
            return SymbolKind::Class; // También podría ser Typedef o Enum, filtraremos por jerarquía
        }
        if (parentType == "call_expression" || parentType == "func_call") {
            // Llamada a función `Func();`
            return SymbolKind::Function; // También Method
        }
        if (parentType == "base_class_list" || parentType == "inheritance_specifier") {
            return SymbolKind::Class; // También Interface, Mixin
        }
        if (parentType == "scoped_identifier") {
            TSNode firstChild = ts_node_child(parent, 0);
            if (ts_node_eq(node, firstChild)) {
                return SymbolKind::Namespace; // También Enum
            } else {
                TSNode grandParent = ts_node_parent(parent);
                if (!ts_node_is_null(grandParent)) {
                    std::string_view gpType = ts_node_type(grandParent);
                    return InferExpectedKind(parent, grandParent, gpType);
                }
            }
        }
        return std::nullopt;
    }

    static int GetKindPriority(SymbolKind kind)
    {
        switch (kind) {
            case SymbolKind::Typedef: return 6;
            case SymbolKind::Class: return 5;
            case SymbolKind::Enum: return 4;
            case SymbolKind::Function: return 3;
            case SymbolKind::Method: return 2;
            default: return 1;
        }
    }

    const Symbol* SymbolResolver::ResolveAt(const Document& doc, const SymbolTable& table, uint32_t line, uint32_t character, std::vector<const Symbol*>* outMultipleResults)
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

        bool isScoped = false;
        std::vector<const Symbol*> globalCandidates;

        auto collectFromNamespace = [&](const std::string& nsText) {
            const Symbol* nsSym = FindNamespace(table, nsText);
            if (nsSym) {
                if (nsSym->kind == SymbolKind::Namespace) {
                    for (const auto& child : nsSym->children) {
                        if (child->name == identText) globalCandidates.push_back(child.get());
                        if (child->kind == SymbolKind::Enum) {
                            for (const auto& eMem : child->children) {
                                if (eMem->name == identText) globalCandidates.push_back(eMem.get());
                            }
                        }
                    }
                } else if (nsSym->kind == SymbolKind::Enum) {
                    for (const auto& child : nsSym->children) {
                        if (child->name == identText) globalCandidates.push_back(child.get());
                    }
                }
            }
        };

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
                    collectFromNamespace(std::string(nsSv.begin(), nsSv.end()));
                    isScoped = true;
                }
            }
        }

        // Namespace resolution (Tree-sitter ERROR workaround for expressions like Math::PI)
        if (!isScoped) {
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
                        collectFromNamespace(std::string(nsSv.begin(), nsSv.end()));
                        isScoped = true;
                    }
                }
            }
        }
        
        // Namespace resolution workaround for Combat::Fire() which produces `ERROR -> type -> scope + datatype -> identifier`
        if (!isScoped && parentType == "datatype")
        {
            TSNode datatypePrevSibling = ts_node_prev_sibling(parent);
            if (!ts_node_is_null(datatypePrevSibling) && std::string_view(ts_node_type(datatypePrevSibling)) == "scope")
            {
                // Child 0 of scope should be the namespace identifier
                TSNode nsNode = ts_node_child(datatypePrevSibling, 0);
                std::string_view nsSv = doc.SourceAt(nsNode);
                collectFromNamespace(std::string(nsSv.begin(), nsSv.end()));
                isScoped = true;
            }
        }

        if (!isScoped && parentType == "variable_declarator")
        {
            TSNode varDecl = ts_node_parent(parent);
            if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
            {
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
                    if (nsText.find("::") != std::string::npos || FindNamespace(table, nsText) != nullptr) {
                        collectFromNamespace(nsText);
                        isScoped = true;
                    }
                }
            }
        }

        if (!isScoped)
        {
            // Local resolution
            if (const Symbol* localSym = table.FindLocalByName(identText))
            {
                if (outMultipleResults) outMultipleResults->push_back(localSym);
                return localSym;
            }

            // Check in using namespaces
            for (const std::string& usingNs : table.GetUsingNamespaces())
            {
                const Symbol* nsSym = FindNamespace(table, usingNs);
                if (nsSym && nsSym->kind == SymbolKind::Namespace) {
                    for (const auto& child : nsSym->children) {
                        if (child->name == identText) globalCandidates.push_back(child.get());
                    }
                }
            }

            // Global search (deep) - find ALL matching globals
            std::vector<Symbol*> allGlobals = table.FindAllGlobalsByName(identText);
            for (Symbol* s : allGlobals) globalCandidates.push_back(s);
            
            for (const auto& [nsName, nsSyms] : table.GetGlobals())
            {
                for (const auto& nsSym : nsSyms)
                {
                    if (nsSym->kind == SymbolKind::Namespace)
                    {
                        auto searchChildren = [&](auto& self, const Symbol* currentNs) -> void {
                            for (const auto& child : currentNs->children)
                            {
                                if (child->name == identText) globalCandidates.push_back(child.get());
                            }
                            for (const auto& child : currentNs->children)
                            {
                                if (child->kind == SymbolKind::Namespace) self(self, child.get());
                            }
                        };
                        searchChildren(searchChildren, nsSym.get());
                    }
                }
            }

            // Deep search for Enum members (AngelScript allows un-prefixed enum members)
            for (const auto& [name, syms] : table.GetGlobals())
            {
                for (const auto& sym : syms)
                {
                    if (sym->kind == SymbolKind::Enum)
                    {
                        for (const auto& child : sym->children)
                        {
                            if (child->name == identText)
                            {
                                globalCandidates.push_back(child.get());
                            }
                        }
                    }
                }
            }
        }
        
        if (!globalCandidates.empty())
        {
            std::optional<SymbolKind> expected = InferExpectedKind(node, parent, parentType);
            const Symbol* bestMatch = nullptr;
            int bestPriority = -1;

            // Remove duplicates (same pointer)
            std::sort(globalCandidates.begin(), globalCandidates.end());
            globalCandidates.erase(std::unique(globalCandidates.begin(), globalCandidates.end()), globalCandidates.end());

            for (const Symbol* cand : globalCandidates)
            {
                if (outMultipleResults) outMultipleResults->push_back(cand);
                
                int candPriority = GetKindPriority(cand->kind);
                
                // If we have an expected kind, boost its priority massively if it matches the category
                if (expected.has_value()) {
                    if (expected.value() == SymbolKind::Class) {
                        if (cand->kind == SymbolKind::Class || cand->kind == SymbolKind::Typedef || cand->kind == SymbolKind::Enum) {
                            candPriority += 100;
                        }
                    } else if (expected.value() == SymbolKind::Function) {
                        if (cand->kind == SymbolKind::Function || cand->kind == SymbolKind::Method) {
                            candPriority += 100;
                        }
                    } else if (expected.value() == SymbolKind::Namespace) {
                        if (cand->kind == SymbolKind::Namespace || cand->kind == SymbolKind::Enum) {
                            candPriority += 100;
                        }
                    }
                }

                if (candPriority > bestPriority) {
                    bestPriority = candPriority;
                    bestMatch = cand;
                }
            }
            
            // If outMultipleResults is null (meaning caller only wants 1 result like Goto Definition)
            // we return bestMatch right away. If it's Hover, we might still return bestMatch but populate outMultipleResults.
            // Wait, we need to continue with scope-aware deep search if globals didn't give us a clear definitive answer?
            // Usually if we find a global we stop.
            return bestMatch;
        }
        
        // Scope-aware Deep member search
        std::string containingClass;
        TSNode climb = ts_node_parent(node);
        while (!ts_node_is_null(climb)) {
            std::string_view ct = ts_node_type(climb);
            if (ct == "class_declaration" || ct == "mixin_declaration" || ct == "interface_declaration") {
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
                // Recursive search through own children AND all base classes (including Mixins)
                auto findInHierarchy = [&](auto& self, const Symbol* cSym) -> const Symbol* {
                    if (!cSym) return nullptr;
                    for (const auto& child : cSym->children) {
                        if (child->name == identText) return child.get();
                    }
                    for (const auto& baseName : cSym->baseClasses) {
                        const Symbol* baseSym = table.FindByNameDeep(baseName);
                        if (const Symbol* found = self(self, baseSym)) return found;
                    }
                    return nullptr;
                };
                if (const Symbol* found = findInHierarchy(findInHierarchy, classSym)) return found;

                // HOST-CLASS SEARCH: Si es un Mixin y no encontramos el miembro,
                // buscar en todas las clases que incluyen este Mixin.
                if (classSym->kind == SymbolKind::Mixin)
                {
                    std::vector<const Symbol*> hosts = table.FindHostClassesOf(containingClass);
                    const Symbol* firstFound = nullptr;
                    for (const Symbol* hostSym : hosts)
                    {
                        auto findMember = [&](auto& self, const Symbol* cSym) -> const Symbol* {
                            if (!cSym) return nullptr;
                            for (const auto& child : cSym->children) {
                                if (child->name == identText) return child.get();
                            }
                            for (const auto& baseName : cSym->baseClasses) {
                                if (baseName == containingClass) continue; // No recursión circular
                                const Symbol* baseSym = table.FindByNameDeep(baseName);
                                if (const Symbol* found = self(self, baseSym)) return found;
                            }
                            return nullptr;
                        };

                        if (const Symbol* found = findMember(findMember, hostSym))
                        {
                            if (outMultipleResults) outMultipleResults->push_back(found);
                            if (!firstFound) firstFound = found;
                        }
                    }
                    if (firstFound) return firstFound;
                }
            }
            // Do not fall back to other classes if we are inside a specific class
            return nullptr;
        }

        // Generic Deep member search (fallback for when not inside a class, e.g. for globals or namespaces)
        for (const auto& [name, syms] : table.GetGlobals()) {
            for (const auto& sym : syms) {
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
