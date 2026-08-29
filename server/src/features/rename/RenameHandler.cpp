#include "features/rename/RenameHandler.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Validates whether a name conforms to valid AngelScript identifier syntax and is not reserved.
         * @param name Name string to validate.
         * @return True if valid identifier.
         */
        bool IsValidIdentifier(std::string_view name)
        {
            if (name.empty())
            {
                return false;
            }

            if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_')
            {
                return false;
            }

            for (size_t i = 1; i < name.size(); ++i)
            {
                if (!std::isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_')
                {
                    return false;
                }
            }

            std::string nameStr(name);
            if (analysis::IsReservedKeyword(nameStr) || analysis::IsPrimitiveTypeName(nameStr))
            {
                return false;
            }

            return true;
        }

        /**
         * @brief Finds the deepest/innermost scope enclosing a given source position.
         */

        /**
         * @brief Searches for the Scope that contains the given LocalDefinition.
         */
        const analysis::Scope *FindScopeDeclaringDefinition(const analysis::Scope *current, const analysis::LocalDefinition &def)
        {
            if (!current)
            {
                return nullptr;
            }

            for (const auto &d : current->definitions)
            {
                if (d.name == def.name &&
                    d.startLine == def.startLine &&
                    d.startCharacter == def.startCharacter &&
                    d.endLine == def.endLine &&
                    d.endCharacter == def.endCharacter)
                {
                    return current;
                }
            }

            for (const auto &child : current->children)
            {
                const analysis::Scope *found = FindScopeDeclaringDefinition(child.get(), def);
                if (found)
                {
                    return found;
                }
            }

            return nullptr;
        }

        /**
         * @brief Checks whether a definition's declaring scope is inside any function/method/lambda body.
         */
        bool IsDeclaredInFunctionScope(const analysis::Scope *defScope)
        {
            for (const analysis::Scope *cur = defScope; cur != nullptr; cur = cur->parent)
            {
                if (cur->isFunctionScope)
                {
                    return true;
                }
            }
            return false;
        }

        std::string GetEnclosingClassName(const analysis::SymbolTable &symbolTable, const std::string &uri, uint32_t line)
        {
            std::string enclosingClass;
            symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if ((sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface) &&
                            sym.fileUri == uri)
                        {
                            if (line >= sym.startLine && line <= sym.endLine)
                            {
                                enclosingClass = sym.name;
                            }
                        }
                    }
                });
            return enclosingClass;
        }

        /**
         * @brief Extracts token text and AST node under cursor with trailing-edge tolerance.
         */
        std::string GetNodeTextAt(const std::string &sourceCode, TSTree *tree, lsp::Position position, TSNode &outNode)
        {
            if (!tree || sourceCode.empty())
            {
                return "";
            }

            TSNode rootNode = ts_tree_root_node(tree);
            TSPoint point = { position.line, position.character };
            TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

            if (ts_node_is_null(node))
            {
                return "";
            }

            std::string_view nodeType = ts_node_type(node);
            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                if (position.character > 0)
                {
                    TSPoint prevPoint = { position.line, position.character - 1 };
                    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
                    if (!ts_node_is_null(prevNode))
                    {
                        std::string_view prevType = ts_node_type(prevNode);
                        if (prevType == "identifier" || prevType == "primitive_type" || prevType == "scoped_identifier")
                        {
                            node = prevNode;
                            nodeType = prevType;
                        }
                    }
                }
            }

            if (nodeType == "scoped_identifier")
            {
                TSNode leaf = ts_node_descendant_for_point_range(node, point, point);
                if (!ts_node_is_null(leaf) && std::string_view(ts_node_type(leaf)) == "identifier")
                {
                    node = leaf;
                    nodeType = "identifier";
                }
            }

            if (nodeType != "identifier" && nodeType != "primitive_type" && nodeType != "scoped_identifier")
            {
                return "";
            }

            uint32_t startByte = ts_node_start_byte(node);
            uint32_t endByte = ts_node_end_byte(node);
            if (startByte >= sourceCode.size() || endByte > sourceCode.size() || startByte >= endByte)
            {
                return "";
            }

            outNode = node;
            return sourceCode.substr(startByte, endByte - startByte);
        }

        enum class TargetKind
        {
            Local,
            ClassMember,
            NamespaceSymbol,
            GlobalSymbol
        };

        struct TargetDescriptor
        {
            TargetKind kind = TargetKind::GlobalSymbol;
            std::string name;
            std::string qualifiedName;

            const analysis::Scope *definingScope = nullptr;
            analysis::LocalDefinition localDef;
            std::string localUri;

            std::string declaringClass;
            std::vector<std::string> relatedClasses;

            std::string declaringNamespace;
        };

        std::vector<std::string> GetAllIndexedFileUris(const analysis::SymbolTable &symbolTable, const std::string &currentUri)
        {
            std::unordered_set<std::string> uriSet;
            if (!currentUri.empty())
            {
                uriSet.insert(currentUri);
            }

            symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (!sym.fileUri.empty())
                        {
                            uriSet.insert(sym.fileUri);
                        }
                    }
                });

            return std::vector<std::string>(uriSet.begin(), uriSet.end());
        }

        /**
         * @brief Resolves target symbol under cursor for renaming/references.
         */
        std::optional<TargetDescriptor> ResolveTargetSymbol(
            const std::string &uri,
            const std::string &sourceCode,
            TSTree *tree,
            lsp::Position position,
            const analysis::SymbolTable &symbolTable,
            const analysis::ScopeIndex &scopeIndex,
            TSNode &outNode)
        {
            std::string nodeText = GetNodeTextAt(sourceCode, tree, position, outNode);
            if (nodeText.empty() || ts_node_is_null(outNode))
            {
                return std::nullopt;
            }

            if (!IsValidIdentifier(nodeText))
            {
                return std::nullopt;
            }

            auto rootScope = scopeIndex.GetRoot(uri);
            TSNode parent = ts_node_parent(outNode);

            TargetDescriptor target;
            target.name = nodeText;

            bool isExplicitMemberAccess = false;
            if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
            {
                TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
                if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, outNode) || ts_node_start_byte(memNode) == ts_node_start_byte(outNode)))
                {
                    isExplicitMemberAccess = true;
                    TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
                    if (!ts_node_is_null(objectNode))
                    {
                        uint32_t objStart = ts_node_start_byte(objectNode);
                        uint32_t objEnd = ts_node_end_byte(objectNode);
                        if (objStart < sourceCode.size() && objEnd <= sourceCode.size())
                        {
                            std::string objText = sourceCode.substr(objStart, objEnd - objStart);
                            std::string receiverTypeName;

                            if (objText == "this")
                            {
                                receiverTypeName = GetEnclosingClassName(symbolTable, uri, position.line);
                            }
                            else if (rootScope)
                            {
                                const analysis::Scope *scope = FindInnermostScope(rootScope.get(), position.line, position.character);
                                if (scope)
                                {
                                    const analysis::LocalDefinition *objDef = analysis::ResolveInScope(scope, objText);
                                    if (objDef && !objDef->typeName.empty())
                                    {
                                        receiverTypeName = analysis::CleanBaseType(objDef->typeName);
                                    }
                                }
                            }

                            if (receiverTypeName.empty())
                            {
                                auto globSyms = symbolTable.FindSymbols(objText);
                                for (const auto &sym : globSyms)
                                {
                                    if (sym.type == analysis::SymbolType::Variable)
                                    {
                                        const auto &var = sym.GetVariable();
                                        if (!var.typeName.empty())
                                        {
                                            receiverTypeName = analysis::CleanBaseType(var.typeName);
                                            break;
                                        }
                                    }
                                }
                            }

                            if (!receiverTypeName.empty())
                            {
                                target.kind = TargetKind::ClassMember;
                                target.declaringClass = receiverTypeName;
                                target.relatedClasses = analysis::GetAllRelatedClasses(receiverTypeName, symbolTable);
                            }
                        }
                    }
                }
            }

            if (!isExplicitMemberAccess && rootScope)
            {
                const analysis::Scope *innerScope = FindInnermostScope(rootScope.get(), position.line, position.character);
                if (innerScope)
                {
                    const analysis::LocalDefinition *matchedDef = nullptr;
                    const analysis::Scope *declScope = nullptr;

                    for (const analysis::Scope *cur = innerScope; cur != nullptr; cur = cur->parent)
                    {
                        for (const auto &d : cur->definitions)
                        {
                            if (d.name == nodeText &&
                                position.line >= d.startLine && position.line <= d.endLine)
                            {
                                matchedDef = &d;
                                declScope = cur;
                                break;
                            }
                        }
                        if (matchedDef)
                        {
                            break;
                        }
                    }

                    if (!matchedDef)
                    {
                        matchedDef = analysis::ResolveInScope(innerScope, nodeText);
                        if (matchedDef)
                        {
                            declScope = FindScopeDeclaringDefinition(rootScope.get(), *matchedDef);
                        }
                    }

                    if (matchedDef && declScope)
                    {
                        if (matchedDef->kind == analysis::LocalDefinitionKind::Parameter ||
                            matchedDef->kind == analysis::LocalDefinitionKind::Variable)
                        {
                            bool isFunctionLocal = IsDeclaredInFunctionScope(declScope);
                            if (isFunctionLocal || matchedDef->kind == analysis::LocalDefinitionKind::Parameter)
                            {
                                target.kind = TargetKind::Local;
                                target.definingScope = declScope;
                                target.localDef = *matchedDef;
                                target.localUri = uri;
                            }
                        }
                        else if (matchedDef->kind == analysis::LocalDefinitionKind::Field ||
                                 matchedDef->kind == analysis::LocalDefinitionKind::Method)
                        {
                            std::string enclosingClass = GetEnclosingClassName(symbolTable, uri, position.line);
                            if (!enclosingClass.empty())
                            {
                                target.kind = TargetKind::ClassMember;
                                target.declaringClass = enclosingClass;
                                target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, symbolTable);
                            }
                        }
                    }
                }
            }

            if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
            {
                auto containers = analysis::GetEnclosingContainers(outNode, sourceCode);
                for (const auto &container : containers)
                {
                    if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
                    {
                        auto hierarchy = analysis::GetAllRelatedClasses(container.qualifiedName, symbolTable);
                        for (const auto &cls : hierarchy)
                        {
                            if (symbolTable.HasSymbol(cls + "::" + nodeText))
                            {
                                target.kind = TargetKind::ClassMember;
                                target.declaringClass = cls;
                                target.relatedClasses = std::move(hierarchy);
                                break;
                            }
                        }
                        if (target.kind == TargetKind::ClassMember)
                        {
                            break;
                        }
                    }
                    else if (container.kind == analysis::ContainerKind::Namespace)
                    {
                        std::string qName = container.qualifiedName + "::" + nodeText;
                        if (symbolTable.HasSymbol(qName))
                        {
                            target.kind = TargetKind::NamespaceSymbol;
                            target.declaringNamespace = container.qualifiedName;
                            target.qualifiedName = qName;
                            break;
                        }
                    }
                }
            }

            if (target.kind == TargetKind::GlobalSymbol)
            {
                symbolTable.ForEachSymbol(
                    [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                    {
                        for (const auto &sym : symbols)
                        {
                            if (sym.fileUri == uri &&
                                position.line >= sym.startLine && position.line <= sym.endLine &&
                                sym.name == nodeText)
                            {
                                if (!sym.containerName.empty())
                                {
                                    auto containerSyms = symbolTable.FindSymbols(sym.containerName);
                                    bool isClassContainer = false;
                                    bool isNamespaceContainer = false;
                                    for (const auto &csym : containerSyms)
                                    {
                                        if (csym.type == analysis::SymbolType::Class || csym.type == analysis::SymbolType::Interface)
                                        {
                                            isClassContainer = true;
                                            break;
                                        }
                                        if (csym.type == analysis::SymbolType::Namespace)
                                        {
                                            isNamespaceContainer = true;
                                        }
                                    }

                                    if (isClassContainer)
                                    {
                                        target.kind = TargetKind::ClassMember;
                                        target.declaringClass = sym.containerName;
                                        target.relatedClasses = analysis::GetAllRelatedClasses(sym.containerName, symbolTable);
                                        return;
                                    }
                                    else if (isNamespaceContainer)
                                    {
                                        target.kind = TargetKind::NamespaceSymbol;
                                        target.declaringNamespace = sym.containerName;
                                        target.qualifiedName = sym.qualifiedName;
                                        return;
                                    }
                                }
                            }
                        }
                    });
            }

            return target;
        }

        /**
         * @brief Collects all occurrences across all documents for a resolved target.
         */
        std::vector<lsp::Location> CollectOccurrences(
            const TargetDescriptor &target,
            const std::string &currentUri,
            const std::string &sourceCode,
            TSTree *tree,
            const analysis::SymbolTable &symbolTable,
            const analysis::ScopeIndex &scopeIndex)
        {
            std::vector<lsp::Location> results;
            std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;

            if (target.kind == TargetKind::Local)
            {
                results.push_back(lsp::Location{
                    lsp::DocumentUri::parse(target.localUri),
                    lsp::Range{
                        lsp::Position{ target.localDef.startLine, target.localDef.startCharacter },
                        lsp::Position{ target.localDef.endLine, target.localDef.endCharacter }
                    }
                });
                seen.insert({ target.localUri, target.localDef.startLine, target.localDef.startCharacter });

                std::function<void(const analysis::Scope *, bool)> collectLocal =
                    [&](const analysis::Scope *scope, bool isRoot)
                    {
                        if (!scope)
                        {
                            return;
                        }

                        if (!isRoot)
                        {
                            for (const auto &def : scope->definitions)
                            {
                                if (def.name == target.name)
                                {
                                    return;
                                }
                            }
                        }

                        for (const auto &ref : scope->references)
                        {
                            if (ref.name == target.name && !ref.isMemberAccess)
                            {
                                if (seen.insert({ target.localUri, ref.startLine, ref.startCharacter }).second)
                                {
                                    results.push_back(lsp::Location{
                                        lsp::DocumentUri::parse(target.localUri),
                                        lsp::Range{
                                            lsp::Position{ ref.startLine, ref.startCharacter },
                                            lsp::Position{ ref.endLine, ref.endCharacter }
                                        }
                                    });
                                }
                            }
                        }

                        for (const auto &child : scope->children)
                        {
                            collectLocal(child.get(), false);
                        }
                    };

                collectLocal(target.definingScope, true);
            }
            else if (target.kind == TargetKind::ClassMember)
            {
                std::unordered_set<std::string> relatedSet(target.relatedClasses.begin(), target.relatedClasses.end());
                if (!target.declaringClass.empty())
                {
                    relatedSet.insert(target.declaringClass);
                }

                for (const auto &cls : relatedSet)
                {
                    auto syms = symbolTable.FindSymbols(cls + "::" + target.name);
                    for (const auto &sym : syms)
                    {
                        if (sym.type != analysis::SymbolType::CallReference)
                        {
                            uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                            uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                            uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                            uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                            if (seen.insert({ sym.fileUri, sL, sC }).second)
                            {
                                results.push_back(lsp::Location{
                                    lsp::DocumentUri::parse(sym.fileUri),
                                    lsp::Range{
                                        lsp::Position{ sL, sC },
                                        lsp::Position{ eL, eC }
                                    }
                                });
                            }
                        }
                    }
                }

                auto allUris = GetAllIndexedFileUris(symbolTable, currentUri);
                for (const auto &fileUri : allUris)
                {
                    auto docScopeRoot = scopeIndex.GetRoot(fileUri);
                    if (!docScopeRoot)
                    {
                        continue;
                    }

                    std::function<void(const analysis::Scope *)> scanScopes =
                        [&](const analysis::Scope *s)
                        {
                            if (!s)
                            {
                                return;
                            }

                            for (const auto &ref : s->references)
                            {
                                if (ref.name != target.name)
                                {
                                    continue;
                                }

                                bool isMatch = false;

                                if (ref.isMemberAccess)
                                {
                                    if (fileUri == currentUri && tree)
                                    {
                                        TSNode rootNode = ts_tree_root_node(tree);
                                        TSPoint pt = { ref.startLine, ref.startCharacter };
                                        TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                        if (!ts_node_is_null(refNode))
                                        {
                                            TSNode exprParent = ts_node_parent(refNode);
                                            if (!ts_node_is_null(exprParent) && std::string_view(ts_node_type(exprParent)) == "member_expression")
                                            {
                                                TSNode objNode = ts_node_child_by_field_name(exprParent, "object", 6);
                                                if (!ts_node_is_null(objNode))
                                                {
                                                    uint32_t oStart = ts_node_start_byte(objNode);
                                                    uint32_t oEnd = ts_node_end_byte(objNode);
                                                    if (oStart < sourceCode.size() && oEnd <= sourceCode.size())
                                                    {
                                                        std::string oText = sourceCode.substr(oStart, oEnd - oStart);
                                                        std::string rType;
                                                        if (oText == "this")
                                                        {
                                                            rType = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                                        }
                                                        else
                                                        {
                                                            const analysis::LocalDefinition *oDef = analysis::ResolveInScope(s, oText);
                                                            if (oDef && !oDef->typeName.empty())
                                                            {
                                                                rType = analysis::CleanBaseType(oDef->typeName);
                                                            }
                                                            else
                                                            {
                                                                auto gSyms = symbolTable.FindSymbols(oText);
                                                                for (const auto &gs : gSyms)
                                                                {
                                                                    if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                                    {
                                                                        rType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                        }

                                                        if (relatedSet.contains(rType))
                                                        {
                                                            isMatch = true;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        isMatch = true;
                                    }
                                }
                                else
                                {
                                    std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                    if (relatedSet.contains(encClass))
                                    {
                                        const analysis::LocalDefinition *localShadow = analysis::ResolveInScope(s, target.name);
                                        if (!localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field || localShadow->kind == analysis::LocalDefinitionKind::Method)
                                        {
                                            isMatch = true;
                                        }
                                    }
                                }

                                if (isMatch)
                                {
                                    if (seen.insert({ fileUri, ref.startLine, ref.startCharacter }).second)
                                    {
                                        results.push_back(lsp::Location{
                                            lsp::DocumentUri::parse(fileUri),
                                            lsp::Range{
                                                lsp::Position{ ref.startLine, ref.startCharacter },
                                                lsp::Position{ ref.endLine, ref.endCharacter }
                                            }
                                        });
                                    }
                                }
                            }

                            for (const auto &child : s->children)
                            {
                                scanScopes(child.get());
                            }
                        };

                    scanScopes(docScopeRoot.get());
                }
            }
            else if (target.kind == TargetKind::NamespaceSymbol)
            {
                // 1. Collect declarations from SymbolTable
                auto syms = symbolTable.FindSymbols(target.qualifiedName);
                for (const auto &sym : syms)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                        uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                        uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                        uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                        if (seen.insert({ sym.fileUri, sL, sC }).second)
                        {
                            results.push_back(lsp::Location{
                                lsp::DocumentUri::parse(sym.fileUri),
                                lsp::Range{
                                    lsp::Position{ sL, sC },
                                    lsp::Position{ eL, eC }
                                }
                            });
                        }
                    }
                }

                // 2. Scan all indexed documents
                auto allUris = GetAllIndexedFileUris(symbolTable, currentUri);
                for (const auto &fileUri : allUris)
                {
                    auto docScopeRoot = scopeIndex.GetRoot(fileUri);
                    if (!docScopeRoot)
                    {
                        continue;
                    }

                    std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
                    symbolTable.ForEachSymbol(
                        [&](const std::string &, const std::vector<analysis::Symbol> &sList)
                        {
                            for (const auto &s : sList)
                            {
                                if (s.type == analysis::SymbolType::Namespace && s.fileUri == fileUri &&
                                    (s.name == target.declaringNamespace || s.qualifiedName == target.declaringNamespace))
                                {
                                    nsRanges.push_back({ s.startLine, s.endLine });
                                }
                            }
                        });

                    std::function<void(const analysis::Scope *)> scanScopes =
                        [&](const analysis::Scope *s)
                        {
                            if (!s)
                            {
                                return;
                            }

                            for (const auto &ref : s->references)
                            {
                                if (ref.name != target.name || ref.isMemberAccess)
                                {
                                    continue;
                                }

                                bool isInsideNamespace = false;
                                for (const auto &nr : nsRanges)
                                {
                                    if (ref.startLine >= nr.first && ref.startLine <= nr.second)
                                    {
                                        isInsideNamespace = true;
                                        break;
                                    }
                                }

                                if (!isInsideNamespace && fileUri == currentUri && tree)
                                {
                                    TSNode rootNode = ts_tree_root_node(tree);
                                    TSPoint pt = { ref.startLine, ref.startCharacter };
                                    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                    if (!ts_node_is_null(refNode))
                                    {
                                        TSNode pNode = ts_node_parent(refNode);
                                        if (!ts_node_is_null(pNode) && std::string_view(ts_node_type(pNode)) == "scoped_identifier")
                                        {
                                            uint32_t pStart = ts_node_start_byte(pNode);
                                            uint32_t pEnd = ts_node_end_byte(pNode);
                                            if (pStart < sourceCode.size() && pEnd <= sourceCode.size())
                                            {
                                                std::string scoped = sourceCode.substr(pStart, pEnd - pStart);
                                                if (scoped == target.qualifiedName)
                                                {
                                                    isInsideNamespace = true;
                                                }
                                            }
                                        }
                                    }
                                }

                                if (!isInsideNamespace)
                                {
                                    continue;
                                }

                                const analysis::LocalDefinition *localDef = analysis::ResolveInScope(s, target.name);
                                if (localDef)
                                {
                                    if (localDef->kind == analysis::LocalDefinitionKind::Parameter ||
                                        localDef->kind == analysis::LocalDefinitionKind::Variable)
                                    {
                                        const analysis::Scope *defScope = FindScopeDeclaringDefinition(docScopeRoot.get(), *localDef);
                                        if (defScope && IsDeclaredInFunctionScope(defScope))
                                        {
                                            continue;
                                        }
                                    }
                                }

                                if (seen.insert({ fileUri, ref.startLine, ref.startCharacter }).second)
                                {
                                    results.push_back(lsp::Location{
                                        lsp::DocumentUri::parse(fileUri),
                                        lsp::Range{
                                            lsp::Position{ ref.startLine, ref.startCharacter },
                                            lsp::Position{ ref.endLine, ref.endCharacter }
                                        }
                                    });
                                }
                            }

                            for (const auto &child : s->children)
                            {
                                scanScopes(child.get());
                            }
                        };

                    scanScopes(docScopeRoot.get());
                }
            }
            else // TargetKind::GlobalSymbol
            {
                auto syms = symbolTable.FindSymbols(target.name);
                for (const auto &sym : syms)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                        uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                        uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                        uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                        if (seen.insert({ sym.fileUri, sL, sC }).second)
                        {
                            results.push_back(lsp::Location{
                                lsp::DocumentUri::parse(sym.fileUri),
                                lsp::Range{
                                    lsp::Position{ sL, sC },
                                    lsp::Position{ eL, eC }
                                }
                            });
                        }
                    }
                }

                symbolTable.ForEachSymbol(
                    [&](const std::string &, const std::vector<analysis::Symbol> &symList)
                    {
                        for (const auto &sym : symList)
                        {
                            if (sym.type == analysis::SymbolType::Enum)
                            {
                                const auto &eSig = sym.GetEnum();
                                for (const auto &mem : eSig.members)
                                {
                                    if (mem.name == target.name)
                                    {
                                        uint32_t sL = sym.startLine;
                                        uint32_t sC = sym.startCharacter;
                                        uint32_t eL = sym.endLine;
                                        uint32_t eC = sym.endCharacter;
                                        if (seen.insert({ sym.fileUri, sL, sC }).second)
                                        {
                                            results.push_back(lsp::Location{
                                                lsp::DocumentUri::parse(sym.fileUri),
                                                lsp::Range{
                                                    lsp::Position{ sL, sC },
                                                    lsp::Position{ eL, eC }
                                                }
                                            });
                                        }
                                    }
                                }
                            }
                        }
                    });

                auto allUris = GetAllIndexedFileUris(symbolTable, currentUri);
                for (const auto &fileUri : allUris)
                {
                    auto docScopeRoot = scopeIndex.GetRoot(fileUri);
                    if (!docScopeRoot)
                    {
                        continue;
                    }

                    std::function<void(const analysis::Scope *)> scanScopes =
                        [&](const analysis::Scope *s)
                        {
                            if (!s)
                            {
                                return;
                            }

                            for (const auto &ref : s->references)
                            {
                                if (ref.name != target.name || ref.isMemberAccess)
                                {
                                    continue;
                                }

                                const analysis::LocalDefinition *localDef = analysis::ResolveInScope(s, target.name);
                                if (localDef)
                                {
                                    if (localDef->kind == analysis::LocalDefinitionKind::Parameter ||
                                        localDef->kind == analysis::LocalDefinitionKind::Variable)
                                    {
                                        const analysis::Scope *defScope = FindScopeDeclaringDefinition(docScopeRoot.get(), *localDef);
                                        if (defScope && IsDeclaredInFunctionScope(defScope))
                                        {
                                            continue;
                                        }
                                    }
                                }

                                if (seen.insert({ fileUri, ref.startLine, ref.startCharacter }).second)
                                {
                                    results.push_back(lsp::Location{
                                        lsp::DocumentUri::parse(fileUri),
                                        lsp::Range{
                                            lsp::Position{ ref.startLine, ref.startCharacter },
                                            lsp::Position{ ref.endLine, ref.endCharacter }
                                        }
                                    });
                                }
                            }

                            for (const auto &child : s->children)
                            {
                                scanScopes(child.get());
                            }
                        };

                    scanScopes(docScopeRoot.get());
                }
            }

            return results;
        }
    }

    std::optional<lsp::PrepareRenameResult> PrepareRename(const PrepareRenameRequest &request)
    {
        if (request.predefinedUris.contains(request.uri))
        {
            return std::nullopt;
        }

        TSNode node{};
        auto target = ResolveTargetSymbol(
            request.uri,
            request.sourceCode,
            request.tree,
            request.position,
            request.symbolTable,
            request.scopeIndex,
            node);

        if (!target.has_value() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        // Check if the symbol definition is located in a predefined header
        if (target->kind == TargetKind::Local)
        {
            if (request.predefinedUris.contains(target->localUri))
            {
                return std::nullopt;
            }
        }
        else if (target->kind == TargetKind::ClassMember)
        {
            bool allPredefined = true;
            bool foundAny = false;
            for (const auto &cls : target->relatedClasses)
            {
                auto syms = request.symbolTable.FindSymbols(cls + "::" + target->name);
                for (const auto &sym : syms)
                {
                    foundAny = true;
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (!allPredefined)
                {
                    break;
                }
            }
            if (foundAny && allPredefined)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto declSyms = request.symbolTable.FindSymbols(target->name);
            if (!declSyms.empty())
            {
                bool allPredefined = true;
                for (const auto &sym : declSyms)
                {
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (allPredefined)
                {
                    return std::nullopt;
                }
            }
        }

        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);

        lsp::PrepareRenamePlaceholder placeholder;
        placeholder.range = lsp::Range{
            lsp::Position{ startPt.row, startPt.column },
            lsp::Position{ endPt.row, endPt.column }
        };
        placeholder.placeholder = target->name;

        return lsp::PrepareRenameResult(placeholder);
    }

    std::optional<lsp::WorkspaceEdit> Rename(const RenameRequest &request)
    {
        if (!IsValidIdentifier(request.newName))
        {
            return std::nullopt;
        }

        if (request.predefinedUris.contains(request.uri))
        {
            return std::nullopt;
        }

        TSNode node{};
        auto target = ResolveTargetSymbol(
            request.uri,
            request.sourceCode,
            request.tree,
            request.position,
            request.symbolTable,
            request.scopeIndex,
            node);

        if (!target.has_value() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        // Reject if target symbol is declared in a predefined header
        if (target->kind == TargetKind::Local)
        {
            if (request.predefinedUris.contains(target->localUri))
            {
                return std::nullopt;
            }
        }
        else if (target->kind == TargetKind::ClassMember)
        {
            bool allPredefined = true;
            bool foundAny = false;
            for (const auto &cls : target->relatedClasses)
            {
                auto syms = request.symbolTable.FindSymbols(cls + "::" + target->name);
                for (const auto &sym : syms)
                {
                    foundAny = true;
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (!allPredefined)
                {
                    break;
                }
            }
            if (foundAny && allPredefined)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto declSyms = request.symbolTable.FindSymbols(target->name);
            if (!declSyms.empty())
            {
                bool allPredefined = true;
                for (const auto &sym : declSyms)
                {
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (allPredefined)
                {
                    return std::nullopt;
                }
            }
        }

        auto occurrences = CollectOccurrences(
            *target,
            request.uri,
            request.sourceCode,
            request.tree,
            request.symbolTable,
            request.scopeIndex);

        if (occurrences.empty())
        {
            return std::nullopt;
        }

        std::map<lsp::DocumentUri, std::vector<lsp::TextEdit>> editsByUri;
        for (const auto &loc : occurrences)
        {
            lsp::TextEdit edit;
            edit.range = loc.range;
            edit.newText = request.newName;
            editsByUri[loc.uri].push_back(std::move(edit));
        }

        lsp::WorkspaceEdit workspaceEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;

        for (auto &[docUri, edits] : editsByUri)
        {
            std::sort(edits.begin(), edits.end(),
                      [](const lsp::TextEdit &a, const lsp::TextEdit &b)
                      {
                          if (a.range.start.line != b.range.start.line)
                          {
                              return a.range.start.line < b.range.start.line;
                          }
                          return a.range.start.character < b.range.start.character;
                      });

            changes[docUri] = std::move(edits);
        }

        workspaceEdit.changes = std::move(changes);
        return workspaceEdit;
    }
}
