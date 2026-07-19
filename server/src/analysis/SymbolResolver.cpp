#include "analysis/SymbolResolver.h"
#include <string_view>
#include <optional>
#include <algorithm>

namespace analysis
{
    static const Symbol *FindNamespace(const SymbolTable &table, const std::string &path)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        size_t end = path.find("::");
        while (end != std::string::npos)
        {
            parts.push_back(path.substr(start, end - start));
            start = end + 2;
            end = path.find("::", start);
        }
        parts.push_back(path.substr(start));

        const Symbol *current = nullptr;
        for (const std::string &part : parts)
        {
            if (!current)
            {
                current = table.FindGlobalByName(part);
            }
            else
            {
                const Symbol *next = nullptr;
                for (const auto &child : current->children)
                {
                    if (child->name == part)
                    {
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
        while (parentType == "scoped_identifier" || parentType == "scoped_type")
        {
            TSNode p = ts_node_parent(parent);
            if (ts_node_is_null(p)) break;
            std::string_view pType = ts_node_type(p);
            if (pType == "scoped_identifier" || pType == "scoped_type")
            {
                node = parent;
                parent = p;
                parentType = pType;
            }
            else
            {
                node = parent;
                parent = p;
                parentType = pType;
                break;
            }
        }

        if (parentType == "type" || parentType == "datatype")
        {
            TSNode gp = ts_node_parent(parent);
            if (!ts_node_is_null(gp))
            {
                TSNode ggp = ts_node_parent(gp);
                if (!ts_node_is_null(ggp) && std::string_view(ts_node_type(ggp)) == "ERROR")
                {
                    for (uint32_t i = 0; i < ts_node_child_count(ggp); i++)
                    {
                        if (std::string_view(ts_node_type(ts_node_child(ggp, i))) == "argument_list")
                        {
                            return SymbolKind::Function;
                        }
                    }
                }
            }
            return SymbolKind::Class; 
        }
        if (parentType == "call_expression" || parentType == "func_call")
        {
            return SymbolKind::Function; 
        }
        
        TSNode nextSibling = ts_node_next_sibling(node);
        if (!ts_node_is_null(nextSibling) && std::string_view(ts_node_type(nextSibling)) == "argument_list")
        {
            return SymbolKind::Function;
        }
        
        if (parentType == "base_class_list" || parentType == "inheritance_specifier") return SymbolKind::Class; 
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

    static std::optional<uint32_t> GetCallArgumentCount(TSNode node, TSNode parent, std::string_view parentType)
    {
        while (parentType == "scoped_identifier" || parentType == "scoped_type")
        {
            TSNode p = ts_node_parent(parent);
            if (ts_node_is_null(p)) break;
            std::string_view pType = ts_node_type(p);
            if (pType == "scoped_identifier" || pType == "scoped_type")
            {
                node = parent;
                parent = p;
                parentType = pType;
            }
            else
            {
                node = parent;
                parent = p;
                parentType = pType;
                break;
            }
        }

        TSNode argList = {0};
        
        if (parentType == "call_expression" || parentType == "func_call")
        {
            argList = ts_node_child_by_field_name(parent, "arguments", 9);
            if (ts_node_is_null(argList))
            {
                for (uint32_t i = 0; i < ts_node_child_count(parent); i++)
                {
                    TSNode child = ts_node_child(parent, i);
                    if (std::string_view(ts_node_type(child)) == "argument_list")
                    {
                        argList = child;
                        break;
                    }
                }
            }
        }
        else
        {
            TSNode nextSibling = ts_node_next_sibling(node);
            if (!ts_node_is_null(nextSibling) && std::string_view(ts_node_type(nextSibling)) == "argument_list")
            {
                argList = nextSibling;
            }
        }
        
        if (!ts_node_is_null(argList))
        {
            uint32_t count = ts_node_child_count(argList);
            uint32_t argCount = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                TSNode child = ts_node_child(argList, i);
                std::string_view type = ts_node_type(child);
                if (type != "(" && type != ")" && type != ",")
                {
                    argCount++;
                }
            }
            return argCount;
        }
        
        return std::nullopt;
    }

    static int GetKindPriority(SymbolKind kind)
    {
        switch (kind)
        {
            case SymbolKind::Typedef: return 6;
            case SymbolKind::Class: return 5;
            case SymbolKind::Constructor: return 4;
            case SymbolKind::Enum: return 4;
            case SymbolKind::Function: return 3;
            case SymbolKind::Method: return 2;
            default: return 1;
        }
    }

    const Symbol *SymbolResolver::ResolveAt(const Document &doc, const SymbolTable &table, uint32_t line, uint32_t character, std::vector<const Symbol *> *outMultipleResults)
    {
        TSNode node = doc.NodeAt(line, character);
        if (ts_node_is_null(node)) return nullptr;

        while (!ts_node_is_null(node))
        {
            std::string_view type = ts_node_type(node);
            if (type == "identifier" || type == "type_identifier") break;
            node = ts_node_parent(node);
        }

        if (ts_node_is_null(node)) return nullptr;

        std::string_view identSv = doc.SourceAt(node);
        std::string identText(identSv.begin(), identSv.end());

        TSNode parent = ts_node_parent(node);
        std::string_view parentType = ts_node_is_null(parent) ? "" : ts_node_type(parent);

        if (parentType == "member_expression")
        {
            if (const Symbol *sym = ResolveMemberAccess(doc, table, node, parent, identText)) return sym;
        }

        if (parentType == "func_declaration")
        {
            if (const Symbol *sym = ResolveConstructorOrDestructor(doc, table, node, parent, identText, line, outMultipleResults)) return sym;
        }

        bool isScoped = false;
        std::vector<const Symbol *> globalCandidates;

        if (parentType == "scoped_identifier" || parentType == "scoped_type")
        {
            if (const Symbol *sym = ResolveScopedIdentifier(doc, table, node, parent, identText, globalCandidates, isScoped))
            {
                if (outMultipleResults) outMultipleResults->push_back(sym);
                return sym;
            }
        }

        if (!isScoped && parentType == "variable_declarator")
        {
            TSNode varDecl = ts_node_parent(parent);
            if (!ts_node_is_null(varDecl) && std::string_view(ts_node_type(varDecl)) == "variable_declaration")
            {
                TSNode tNode = ts_node_child_by_field_name(varDecl, "var_type", 8);
                if (ts_node_is_null(tNode))
                {
                    for (uint32_t j = 0; j < ts_node_child_count(varDecl); j++)
                    {
                        TSNode child = ts_node_child(varDecl, j);
                        if (std::string_view(ts_node_type(child)) == "type")
                        {
                            tNode = child;
                            break;
                        }
                    }
                }
                
                if (!ts_node_is_null(tNode))
                {
                    std::string_view nsSv = doc.SourceAt(tNode);
                    std::string nsText(nsSv.begin(), nsSv.end());
                    const Symbol *maybeSym = FindNamespace(table, nsText);
                    if (maybeSym && maybeSym->kind == SymbolKind::Namespace)
                    {
                        for (const auto &child : maybeSym->children)
                        {
                            if (child->name == identText) globalCandidates.push_back(child.get());
                            if (child->kind == SymbolKind::Enum)
                            {
                                for (const auto &eMem : child->children)
                                {
                                    if (eMem->name == identText) globalCandidates.push_back(eMem.get());
                                }
                            }
                        }
                        isScoped = true;
                    }
                }
            }
        }

        if (!isScoped)
        {
            if (const Symbol *localSym = table.FindLocalByNameAt(identText, line, character))
            {
                if (outMultipleResults) outMultipleResults->push_back(localSym);
                return localSym;
            }

            for (const std::string &usingNs : table.GetUsingNamespaces())
            {
                const Symbol *nsSym = FindNamespace(table, usingNs);
                if (nsSym && nsSym->kind == SymbolKind::Namespace)
                {
                    for (const auto &child : nsSym->children)
                    {
                        if (child->name == identText) globalCandidates.push_back(child.get());
                    }
                }
            }

            std::vector<Symbol *> allGlobals = table.FindAllGlobalsByName(identText);
            for (Symbol *s : allGlobals) globalCandidates.push_back(s);
            
            for (const auto &[nsName, nsSyms] : table.GetGlobals())
            {
                for (const auto &nsSym : nsSyms)
                {
                    if (nsSym->kind == SymbolKind::Namespace)
                    {
                        auto searchChildren = [&](auto &self, const Symbol *currentNs) -> void
                        {
                            for (const auto &child : currentNs->children)
                            {
                                if (child->name == identText) globalCandidates.push_back(child.get());
                            }
                            for (const auto &child : currentNs->children)
                            {
                                if (child->kind == SymbolKind::Namespace) self(self, child.get());
                            }
                        };
                        searchChildren(searchChildren, nsSym.get());
                    }
                }
            }

            for (const auto &[name, syms] : table.GetGlobals())
            {
                for (const auto &sym : syms)
                {
                    if (sym->kind == SymbolKind::Enum)
                    {
                        for (const auto &child : sym->children)
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
            return FilterAndScoreCandidates(doc, node, parent, parentType, globalCandidates, outMultipleResults);
        }
        
        if (const Symbol *implicit = ResolveImplicitMember(doc, table, node, identText, outMultipleResults))
        {
            return implicit;
        }

        for (const auto &[name, syms] : table.GetGlobals())
        {
            for (const auto &sym : syms)
            {
                if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Namespace)
                {
                    for (const auto &child : sym->children)
                    {
                        if (child->name == identText) return child.get();
                        for (const auto &grandchild : child->children)
                        {
                            if (grandchild->name == identText) return grandchild.get();
                        }
                    }
                }
            }
        }

        return nullptr;
    }

    const Symbol *SymbolResolver::ResolveMemberAccess(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText)
    {
        TSNode memberNode = ts_node_child_by_field_name(parent, "member", 6);
        TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
        
        if (!ts_node_is_null(memberNode) && ts_node_eq(node, memberNode))
        {
            std::string_view objSv = doc.SourceAt(objectNode);
            std::string objText(objSv.begin(), objSv.end());
                
            const Symbol *objSym = table.FindLocalByName(objText);
            if (!objSym) objSym = table.FindGlobalByName(objText);
                    
            if (objSym)
            {
                std::string typeName = CleanTypeName(objSym->typeInfo);
                
                const Symbol *classSym = nullptr;
                if (typeName.find("::") != std::string::npos)
                {
                    classSym = FindNamespace(table, typeName);
                }
                else
                {
                    classSym = table.FindByNameDeep(typeName);
                }
                
                if (classSym)
                {
                    auto findMember = [&](auto &self, const Symbol *cSym) -> const Symbol *
                    {
                        if (!cSym) return nullptr;
                        for (const auto &child : cSym->children)
                        {
                            if (child->name == identText) return child.get();
                        }
                        for (const auto &baseName : cSym->baseClasses)
                        {
                            const Symbol *baseSym = table.FindByNameDeep(baseName);
                            if (baseSym)
                            {
                                if (const Symbol *found = self(self, baseSym)) return found;
                            }
                        }
                        return nullptr;
                    };
                    
                    if (const Symbol *found = findMember(findMember, classSym))
                    {
                        return found;
                    }
                }
            }
        }
        return nullptr;
    }

    const Symbol *SymbolResolver::ResolveConstructorOrDestructor(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, uint32_t line, std::vector<const Symbol *> *outMultipleResults)
    {
        bool hasReturnType = false;
        for (uint32_t i = 0; i < ts_node_child_count(parent); i++)
        {
            TSNode child = ts_node_child(parent, i);
            std::string_view type = ts_node_type(child);
            if (type == "return_type" || type == "type" || type == "datatype")
            {
                hasReturnType = true;
                break;
            }
        }
        
        if (!hasReturnType)
        {
            bool isDestructor = false;
            TSNode prev = ts_node_prev_sibling(node);
            if (!ts_node_is_null(prev) && std::string_view(ts_node_type(prev)) == "~")
            {
                isDestructor = true;
            }
            
            std::string targetName = isDestructor ? ("~" + identText) : identText;
            
            const Symbol *foundSym = nullptr;
            TSNode scopeNode = parent;
            while (!ts_node_is_null(scopeNode))
            {
                if (std::string_view(ts_node_type(scopeNode)) == "class_declaration")
                {
                    TSNode classNameNode = ts_node_child_by_field_name(scopeNode, "name", 4);
                    if (!ts_node_is_null(classNameNode))
                    {
                        std::string_view cNameSv = doc.SourceAt(classNameNode);
                        std::string cName(cNameSv.begin(), cNameSv.end());
                        const Symbol *classSym = table.FindByNameDeep(cName);
                        if (classSym)
                        {
                            for (const auto &child : classSym->children)
                            {
                                if (child->name == targetName && (child->kind == SymbolKind::Constructor || child->kind == SymbolKind::Destructor))
                                {
                                    bool inRange = (child->selectionRange.start.line == (uint32_t)line);
                                    if (inRange)
                                    {
                                        foundSym = child.get();
                                        break;
                                    }
                                    if (!foundSym)
                                    {
                                        foundSym = child.get(); 
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                scopeNode = ts_node_parent(scopeNode);
            }
            
            if (foundSym)
            {
                if (outMultipleResults) outMultipleResults->push_back(foundSym);
                return foundSym;
            }
        }
        return nullptr;
    }

    const Symbol *SymbolResolver::ResolveScopedIdentifier(const Document &doc, const SymbolTable &table, TSNode node, TSNode parent, const std::string &identText, std::vector<const Symbol *> &globalCandidates, bool &outIsScoped)
    {
        TSNode topmostScope = parent;
        while (!ts_node_is_null(ts_node_parent(topmostScope)))
        {
            std::string_view pType = ts_node_type(ts_node_parent(topmostScope));
            if (pType == "scoped_identifier" || pType == "scoped_type")
            {
                topmostScope = ts_node_parent(topmostScope);
            }
            else
            {
                break;
            }
        }

        std::vector<std::pair<std::string, TSNode>> pathNodes;
        auto collectScopedIdentifiers = [&](TSNode scopedNode, auto &self) -> void
        {
            uint32_t count = ts_node_child_count(scopedNode);
            for (uint32_t i = 0; i < count; i++)
            {
                TSNode child = ts_node_child(scopedNode, i);
                std::string_view childType = ts_node_type(child);
                if (childType == "identifier" || childType == "type_identifier")
                {
                    std::string_view text = doc.SourceAt(child);
                    pathNodes.push_back({std::string(text.begin(), text.end()), child});
                }
                else if (childType == "scoped_identifier" || childType == "scoped_type")
                {
                    self(child, self);
                }
            }
        };
        collectScopedIdentifiers(topmostScope, collectScopedIdentifiers);

        int hoveredIndex = -1;
        for (size_t i = 0; i < pathNodes.size(); i++)
        {
            if (ts_node_eq(pathNodes[i].second, node))
            {
                hoveredIndex = i;
                break;
            }
        }

        if (hoveredIndex != -1)
        {
            if (hoveredIndex == (int)pathNodes.size() - 1)
            {
                std::string nsPath = "";
                for (int i = 0; i < hoveredIndex; i++)
                {
                    if (!nsPath.empty()) nsPath += "::";
                    nsPath += pathNodes[i].first;
                }
                if (!nsPath.empty())
                {
                    const Symbol *nsSym = FindNamespace(table, nsPath);
                    if (nsSym)
                    {
                        if (nsSym->kind == SymbolKind::Namespace)
                        {
                            for (const auto &child : nsSym->children)
                            {
                                if (child->name == identText) globalCandidates.push_back(child.get());
                                if (child->kind == SymbolKind::Enum)
                                {
                                    for (const auto &eMem : child->children)
                                    {
                                        if (eMem->name == identText) globalCandidates.push_back(eMem.get());
                                    }
                                }
                            }
                        }
                        else if (nsSym->kind == SymbolKind::Enum)
                        {
                            for (const auto &child : nsSym->children)
                            {
                                if (child->name == identText) globalCandidates.push_back(child.get());
                            }
                        }
                    }
                    outIsScoped = true;
                }
            }
            else
            {
                std::string nsPath = "";
                for (int i = 0; i <= hoveredIndex; i++)
                {
                    if (!nsPath.empty()) nsPath += "::";
                    nsPath += pathNodes[i].first;
                }
                const Symbol *nsSym = FindNamespace(table, nsPath);
                if (nsSym) return nsSym;
            }
        }
        return nullptr;
    }

    const Symbol *SymbolResolver::ResolveImplicitMember(const Document &doc, const SymbolTable &table, TSNode node, const std::string &identText, std::vector<const Symbol *> *outMultipleResults)
    {
        std::string containingClass;
        TSNode climb = ts_node_parent(node);
        while (!ts_node_is_null(climb))
        {
            std::string_view ct = ts_node_type(climb);
            if (ct == "class_declaration" || ct == "mixin_declaration" || ct == "interface_declaration")
            {
                TSNode nameNode = ts_node_child_by_field_name(climb, "name", 4);
                if (!ts_node_is_null(nameNode))
                {
                    std::string_view sv = doc.SourceAt(nameNode);
                    containingClass = std::string(sv.begin(), sv.end());
                }
                break;
            }
            climb = ts_node_parent(climb);
        }

        if (!containingClass.empty())
        {
            const Symbol *classSym = table.FindByNameDeep(containingClass);
            if (classSym)
            {
                if (identText == containingClass)
                {
                    return classSym;
                }
                
                auto findInHierarchy = [&](auto &self, const Symbol *cSym) -> const Symbol *
                {
                    if (!cSym) return nullptr;
                    for (const auto &child : cSym->children)
                    {
                        if (child->name == identText) return child.get();
                    }
                    for (const auto &baseName : cSym->baseClasses)
                    {
                        const Symbol *baseSym = table.FindByNameDeep(baseName);
                        if (const Symbol *found = self(self, baseSym)) return found;
                    }
                    return nullptr;
                };
                
                if (const Symbol *found = findInHierarchy(findInHierarchy, classSym)) return found;

                if (classSym->kind == SymbolKind::Mixin)
                {
                    std::vector<const Symbol *> hosts = table.FindHostClassesOf(containingClass);
                    const Symbol *firstFound = nullptr;
                    for (const Symbol *hostSym : hosts)
                    {
                        auto findMember = [&](auto &self, const Symbol *cSym) -> const Symbol *
                        {
                            if (!cSym) return nullptr;
                            for (const auto &child : cSym->children)
                            {
                                if (child->name == identText) return child.get();
                            }
                            for (const auto &baseName : cSym->baseClasses)
                            {
                                if (baseName == containingClass) continue;
                                const Symbol *baseSym = table.FindByNameDeep(baseName);
                                if (const Symbol *found = self(self, baseSym)) return found;
                            }
                            return nullptr;
                        };

                        if (const Symbol *found = findMember(findMember, hostSym))
                        {
                            if (outMultipleResults) outMultipleResults->push_back(found);
                            if (!firstFound) firstFound = found;
                        }
                    }
                    if (firstFound) return firstFound;
                }
            }
        }
        return nullptr;
    }

    const Symbol *SymbolResolver::FilterAndScoreCandidates(const Document &doc, TSNode node, TSNode parent, std::string_view parentType, std::vector<const Symbol *> &globalCandidates, std::vector<const Symbol *> *outMultipleResults)
    {
        std::optional<SymbolKind> expected = InferExpectedKind(node, parent, parentType);
        std::optional<uint32_t> argCount = GetCallArgumentCount(node, parent, parentType);
        
        const Symbol *bestMatch = nullptr;
        int bestPriority = -1;

        std::sort(globalCandidates.begin(), globalCandidates.end());
        globalCandidates.erase(std::unique(globalCandidates.begin(), globalCandidates.end()), globalCandidates.end());

        for (const Symbol *cand : globalCandidates)
        {
            if (outMultipleResults) outMultipleResults->push_back(cand);
            
            int candPriority = GetKindPriority(cand->kind);
            
            if (expected.has_value())
            {
                if (expected.value() == cand->kind)
                {
                    candPriority += 500;
                }

                if (expected.value() == SymbolKind::Class)
                {
                    if (cand->kind == SymbolKind::Class || cand->kind == SymbolKind::Typedef || cand->kind == SymbolKind::Enum)
                    {
                        candPriority += 100;
                    }
                }
                else if (expected.value() == SymbolKind::Function)
                {
                    if (cand->kind == SymbolKind::Function || cand->kind == SymbolKind::Method || cand->kind == SymbolKind::Constructor)
                    {
                        candPriority += 100;
                        if (cand->kind == SymbolKind::Constructor && argCount.has_value())
                        {
                            if (cand->params.size() == argCount.value())
                            {
                                candPriority += 200;
                            }
                        }
                    }
                }
                else if (expected.value() == SymbolKind::Namespace)
                {
                    if (cand->kind == SymbolKind::Namespace || cand->kind == SymbolKind::Enum)
                    {
                        candPriority += 100;
                    }
                }
            }

            if (candPriority > bestPriority)
            {
                bestPriority = candPriority;
                bestMatch = cand;
            }
        }
        
        if (bestMatch && bestMatch->kind == SymbolKind::Class &&
            expected.has_value() && expected.value() == SymbolKind::Function)
        {
            const Symbol *bestCtor = nullptr;
            int bestCtorScore = -1;
            for (const auto &child : bestMatch->children)
            {
                if (child->kind != SymbolKind::Constructor) continue;
                int score = 0;
                if (argCount.has_value())
                {
                    if (child->params.size() == argCount.value())
                        score += 200;
                    else
                        score -= 50;
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

    std::string SymbolResolver::CleanTypeName(std::string_view raw)
    {
        std::string result(raw);
        
        auto stripPrefix = [&result](const std::string &prefix)
        {
            if (result.starts_with(prefix))
            {
                result = result.substr(prefix.length());
                while (!result.empty() && result[0] == ' ')
                    result = result.substr(1);
            }
        };

        stripPrefix("const ");
        stripPrefix("inout ");
        stripPrefix("in ");
        stripPrefix("out ");
        
        auto removeChar = [&result](char c)
        {
            size_t pos;
            while ((pos = result.find(c)) != std::string::npos)
                result.erase(pos, 1);
        };
        
        removeChar('@');
        removeChar('&');
        
        size_t arrPos;
        while ((arrPos = result.find("[]")) != std::string::npos)
            result.erase(arrPos, 2);
            
        while (!result.empty() && result.back() == ' ')
            result.pop_back();

        return result;
    }

    std::string SymbolResolver::EvaluateExpressionType(const Document &doc, const SymbolTable &table, TSNode exprNode)
    {
        if (ts_node_is_null(exprNode)) return "";

        std::string_view type = ts_node_type(exprNode);

        if (type == "number_literal")
        {
            std::string_view val = doc.SourceAt(exprNode);
            if (val.find('.') != std::string_view::npos || val.find('f') != std::string_view::npos)
                return "float";
            return "int";
        }
        if (type == "string_literal")
        {
            return "string";
        }
        if (type == "true" || type == "false")
        {
            return "bool";
        }
        if (type == "identifier")
        {
            TSPoint pos = ts_node_start_point(exprNode);
            if (const Symbol* sym = ResolveAt(doc, table, pos.row, pos.column))
            {
                if (sym->kind == SymbolKind::Variable || sym->kind == SymbolKind::Property || sym->kind == SymbolKind::Parameter)
                {
                    return sym->typeInfo;
                }
                else if (sym->kind == SymbolKind::Class || sym->kind == SymbolKind::Enum)
                {
                    return sym->name;
                }
            }
        }
        if (type == "call_expression")
        {
            TSNode funcNode = ts_node_child(exprNode, 0);
            if (!ts_node_is_null(funcNode))
            {
                TSPoint pos = ts_node_start_point(funcNode);
                if (std::string_view(ts_node_type(funcNode)) == "member_expression")
                {
                    TSNode fieldNode = ts_node_child_by_field_name(funcNode, "field", 5);
                    if (!ts_node_is_null(fieldNode))
                    {
                        pos = ts_node_start_point(fieldNode);
                    }
                }

                if (const Symbol* sym = ResolveAt(doc, table, pos.row, pos.column))
                {
                    if (sym->kind == SymbolKind::Method || sym->kind == SymbolKind::Function)
                    {
                        return CleanTypeName(sym->typeInfo);
                    }
                }
            }
        }
        if (type == "member_expression")
        {
            TSNode fieldNode = ts_node_child_by_field_name(exprNode, "field", 5);
            if (!ts_node_is_null(fieldNode))
            {
                TSPoint pos = ts_node_start_point(fieldNode);
                if (const Symbol* sym = ResolveAt(doc, table, pos.row, pos.column))
                {
                    if (sym->kind == SymbolKind::Property || sym->kind == SymbolKind::Variable)
                        return sym->typeInfo;
                }
            }
        }
        if (type == "cast_expression")
        {
            TSNode typeNode = ts_node_child_by_field_name(exprNode, "type", 4);
            if (!ts_node_is_null(typeNode))
            {
                std::string_view castType = doc.SourceAt(typeNode);
                return std::string(castType.begin(), castType.end());
            }
        }

        return "";
    }
}
