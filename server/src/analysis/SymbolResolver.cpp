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
                    if (child->name == part) {
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
        // Climb out of recursive scoped_identifiers to see what the entire scope is being used for
        while (parentType == "scoped_identifier" || parentType == "scoped_type") {
            // Wait, if we are evaluating the LEFT side of a scope (e.g. `Engine` in `Engine::Math`), we know it expects a Namespace or Enum.
            // But we can check that by seeing if the hovered node is the LAST node in the topmost scope.
            // For now, let's just climb up.
            TSNode p = ts_node_parent(parent);
            if (ts_node_is_null(p)) break;
            std::string_view pType = ts_node_type(p);
            if (pType == "scoped_identifier" || pType == "scoped_type") {
                node = parent;
                parent = p;
                parentType = pType;
            } else {
                node = parent;
                parent = p;
                parentType = pType;
                break;
            }
        }

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
            return SymbolKind::Class; 
        }
        if (parentType == "call_expression" || parentType == "func_call") {
            return SymbolKind::Function; 
        }
        
        TSNode nextSibling = ts_node_next_sibling(node);
        if (!ts_node_is_null(nextSibling) && std::string_view(ts_node_type(nextSibling)) == "argument_list") {
            return SymbolKind::Function;
        }
        
        if (parentType == "base_class_list" || parentType == "inheritance_specifier") {
            return SymbolKind::Class; 
        }

        if (parentType == "class_declaration") return SymbolKind::Class;
        if (parentType == "enum_declaration") return SymbolKind::Enum;
        if (parentType == "typedef_declaration") return SymbolKind::Typedef;
        if (parentType == "funcdef_declaration") return SymbolKind::Funcdef;
        if (parentType == "func_declaration") return SymbolKind::Function;
        if (parentType == "interface_declaration") return SymbolKind::Interface;
        if (parentType == "mixin_declaration") return SymbolKind::Mixin;
        if (parentType == "namespace_declaration") return SymbolKind::Namespace;
        
        return std::nullopt;
    }

    // Returns argument count if this node is the callee of a call expression
    static std::optional<uint32_t> GetCallArgumentCount(TSNode node, TSNode parent, std::string_view parentType)
    {
        while (parentType == "scoped_identifier" || parentType == "scoped_type") {
            TSNode p = ts_node_parent(parent);
            if (ts_node_is_null(p)) break;
            std::string_view pType = ts_node_type(p);
            if (pType == "scoped_identifier" || pType == "scoped_type") {
                node = parent;
                parent = p;
                parentType = pType;
            } else {
                node = parent;
                parent = p;
                parentType = pType;
                break;
            }
        }

        TSNode argList = {0};
        
        if (parentType == "call_expression" || parentType == "func_call") {
            argList = ts_node_child_by_field_name(parent, "arguments", 9);
            if (ts_node_is_null(argList)) {
                for (uint32_t i = 0; i < ts_node_child_count(parent); i++) {
                    TSNode child = ts_node_child(parent, i);
                    if (std::string_view(ts_node_type(child)) == "argument_list") {
                        argList = child;
                        break;
                    }
                }
            }
        } else {
            TSNode nextSibling = ts_node_next_sibling(node);
            if (!ts_node_is_null(nextSibling) && std::string_view(ts_node_type(nextSibling)) == "argument_list") {
                argList = nextSibling;
            }
        }
        
        if (!ts_node_is_null(argList)) {
            // Count arguments based on comma nodes or child count heuristics
            uint32_t count = ts_node_child_count(argList);
            uint32_t argCount = 0;
            for (uint32_t i = 0; i < count; i++) {
                TSNode child = ts_node_child(argList, i);
                std::string_view type = ts_node_type(child);
                if (type != "(" && type != ")" && type != ",") {
                    argCount++;
                }
            }
            return argCount;
        }
        
        return std::nullopt;
    }

    static int GetKindPriority(SymbolKind kind)
    {
        switch (kind) {
            case SymbolKind::Typedef: return 6;
            case SymbolKind::Class: return 5;
            case SymbolKind::Constructor: return 4;
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

        // Check if we are hovering a constructor or destructor
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
                        
                        // 3. Find the type (class) symbol
                        const Symbol* classSym = nullptr;
                        if (typeName.find("::") != std::string::npos) {
                            classSym = FindNamespace(table, typeName);
                        } else {
                            classSym = table.FindByNameDeep(typeName);
                        }
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

        // Check if we are hovering a constructor or destructor
        if (parentType == "func_declaration")
        {
            // If the func_declaration doesn't have a return_type, it's a constructor or destructor.
            bool hasReturnType = false;
            for (uint32_t i = 0; i < ts_node_child_count(parent); i++) {
                TSNode child = ts_node_child(parent, i);
                if (std::string_view(ts_node_type(child)) == "return_type" || 
                    std::string_view(ts_node_type(child)) == "type" || 
                    std::string_view(ts_node_type(child)) == "datatype") {
                    hasReturnType = true;
                    break;
                }
            }
            if (!hasReturnType) {
                // Determine if it's a destructor by checking for ~
                bool isDestructor = false;
                TSNode prev = ts_node_prev_sibling(node);
                if (!ts_node_is_null(prev) && std::string_view(ts_node_type(prev)) == "~") {
                    isDestructor = true;
                }
                
                std::string targetName = isDestructor ? ("~" + identText) : identText;
                
                // We should look for Constructor or Destructor first
                const Symbol* foundSym = nullptr;
                // Climb scopes to find the enclosing class and look inside its children
                TSNode scopeNode = parent;
                while (!ts_node_is_null(scopeNode)) {
                    if (std::string_view(ts_node_type(scopeNode)) == "class_declaration") {
                        TSNode classNameNode = ts_node_child_by_field_name(scopeNode, "name", 4);
                        if (!ts_node_is_null(classNameNode)) {
                            std::string_view cNameSv = doc.SourceAt(classNameNode);
                            std::string cName(cNameSv.begin(), cNameSv.end());
                            const Symbol* classSym = table.FindByNameDeep(cName);
                            if (classSym) {
                                for (const auto& child : classSym->children) {
                                    if (child->name == targetName && (child->kind == SymbolKind::Constructor || child->kind == SymbolKind::Destructor)) {
                                        bool inRange = (child->selectionRange.start.line == (uint32_t)line);
                                        if (inRange) {
                                            foundSym = child.get();
                                            break;
                                        }
                                        if (!foundSym) {
                                            foundSym = child.get(); // fallback
                                        }
                                    }
                                }
                            }
                        }
                        break;
                    }
                    scopeNode = ts_node_parent(scopeNode);
                }
                
                if (foundSym) {
                    if (outMultipleResults) outMultipleResults->push_back(foundSym);
                    return foundSym;
                }
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

        // Namespace resolution (Valid scoped_identifier or scoped_type)
        if (parentType == "scoped_identifier" || parentType == "scoped_type")
        {
            TSNode topmostScope = parent;
            while (!ts_node_is_null(ts_node_parent(topmostScope)))
            {
                std::string_view pType = ts_node_type(ts_node_parent(topmostScope));
                if (pType == "scoped_identifier" || pType == "scoped_type") {
                    topmostScope = ts_node_parent(topmostScope);
                } else {
                    break;
                }
            }

            std::vector<std::pair<std::string, TSNode>> pathNodes;
            auto collectScopedIdentifiers = [&](TSNode scopedNode, auto& self) -> void {
                uint32_t count = ts_node_child_count(scopedNode);
                for (uint32_t i = 0; i < count; i++) {
                    TSNode child = ts_node_child(scopedNode, i);
                    std::string_view childType = ts_node_type(child);
                    if (childType == "identifier" || childType == "type_identifier") {
                        std::string_view text = doc.SourceAt(child);
                        pathNodes.push_back({std::string(text.begin(), text.end()), child});
                    } else if (childType == "scoped_identifier" || childType == "scoped_type") {
                        self(child, self);
                    }
                }
            };
            collectScopedIdentifiers(topmostScope, collectScopedIdentifiers);

            int hoveredIndex = -1;
            for (size_t i = 0; i < pathNodes.size(); i++) {
                if (ts_node_eq(pathNodes[i].second, node)) {
                    hoveredIndex = i;
                    break;
                }
            }

            if (hoveredIndex != -1) {
                if (hoveredIndex == (int)pathNodes.size() - 1) {
                    std::string nsPath = "";
                    for (int i = 0; i < hoveredIndex; i++) {
                        if (!nsPath.empty()) nsPath += "::";
                        nsPath += pathNodes[i].first;
                    }
                    if (!nsPath.empty()) {
                        collectFromNamespace(nsPath);
                        isScoped = true;
                    }
                } else {
                    // Hovering over an intermediate namespace, e.g. `Engine` or `Math`
                    std::string nsPath = "";
                    for (int i = 0; i <= hoveredIndex; i++) {
                        if (!nsPath.empty()) nsPath += "::";
                        nsPath += pathNodes[i].first;
                    }
                    const Symbol* nsSym = FindNamespace(table, nsPath);
                    if (nsSym) {
                        if (outMultipleResults) outMultipleResults->push_back(nsSym);
                        return nsSym;
                    }
                    return nullptr;
                }
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
                    const Symbol* maybeSym = FindNamespace(table, nsText);
                    if (maybeSym && maybeSym->kind == SymbolKind::Namespace) {
                        collectFromNamespace(nsText);
                        isScoped = true;
                    }
                }
            }
        }

        if (!isScoped)
        {
            // Local resolution
            if (const Symbol* localSym = table.FindLocalByNameAt(identText, line, character))
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
            std::optional<uint32_t> argCount = GetCallArgumentCount(node, parent, parentType);
            
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
                    if (expected.value() == cand->kind) {
                        candPriority += 500; // Exact match of expected kind gets highest priority
                    }

                    if (expected.value() == SymbolKind::Class) {
                        if (cand->kind == SymbolKind::Class || cand->kind == SymbolKind::Typedef || cand->kind == SymbolKind::Enum) {
                            candPriority += 100;
                        }
                    } else if (expected.value() == SymbolKind::Function) {
                        if (cand->kind == SymbolKind::Function || cand->kind == SymbolKind::Method || cand->kind == SymbolKind::Constructor) {
                            candPriority += 100;
                            // Constructor arity matching (Bug 2)
                            if (cand->kind == SymbolKind::Constructor && argCount.has_value()) {
                                if (cand->params.size() == argCount.value()) {
                                    candPriority += 200; // Perfect match
                                }
                            }
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
            
            // Constructor redirection: if the resolved symbol is a Class but the context is a call
            // expression (e.g. `Vector3(x, y, z)`), the user is actually calling a constructor.
            // Find the best matching constructor inside the class by argument count.
            if (bestMatch && bestMatch->kind == SymbolKind::Class &&
                expected.has_value() && expected.value() == SymbolKind::Function)
            {
                const Symbol* bestCtor = nullptr;
                int bestCtorScore = -1;
                for (const auto& child : bestMatch->children)
                {
                    if (child->kind != SymbolKind::Constructor) continue;
                    int score = 0;
                    if (argCount.has_value())
                    {
                        if (child->params.size() == argCount.value())
                            score += 200; // Perfect arity match
                        else
                            score -= 50;  // Penalize wrong arity
                    }
                    if (score > bestCtorScore)
                    {
                        bestCtorScore = score;
                        bestCtor = child.get();
                    }
                }
                if (bestCtor) bestMatch = bestCtor;
            }
            
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
