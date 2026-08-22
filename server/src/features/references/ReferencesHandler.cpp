#include "features/references/ReferencesHandler.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Checks if a given line and character position falls within a scope's range.
         * @param scope Target scope.
         * @param line 0-based line number.
         * @param character 0-based character offset.
         * @return True if position is inside scope boundaries.
         */
        bool IsInsideScope(const analysis::Scope &scope, uint32_t line, uint32_t character)
        {
            if (line < scope.startLine || line > scope.endLine)
            {
                return false;
            }
            if (line == scope.startLine && character < scope.startCharacter)
            {
                return false;
            }
            if (line == scope.endLine && character > scope.endCharacter)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief Finds the deepest/innermost scope enclosing a given source position.
         * @param root Root scope of the document.
         * @param line 0-based line number.
         * @param character 0-based character offset.
         * @return Innermost Scope pointer or nullptr if position is outside root.
         */
        const analysis::Scope *FindInnermostScope(const analysis::Scope *root, uint32_t line, uint32_t character)
        {
            if (!root || !IsInsideScope(*root, line, character))
            {
                return nullptr;
            }

            const analysis::Scope *current = root;
            bool foundChild = true;
            while (foundChild)
            {
                foundChild = false;
                for (const auto &child : current->children)
                {
                    if (child && IsInsideScope(*child, line, character))
                    {
                        current = child.get();
                        foundChild = true;
                        break;
                    }
                }
            }
            return current;
        }

        /**
         * @brief Recursively searches for the Scope that contains the given LocalDefinition.
         * @param current Current scope node in the tree.
         * @param def Target local definition to match.
         * @return Pointer to declaring Scope or nullptr if not found.
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

        /**
         * @brief Resolves the enclosing class name for a given source position.
         * @param symbolTable Symbol table to look up class definitions.
         * @param uri Document file URI.
         * @param line 0-based line number.
         * @return Enclosing class name or empty string if not in a class.
         */
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
         * @param sourceCode Document source text.
         * @param tree Tree-sitter AST.
         * @param position Cursor position.
         * @param outNode Output TSNode.
         * @return Token text or empty string if not an identifier.
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

            // Local variable / parameter
            const analysis::Scope *definingScope = nullptr;
            analysis::LocalDefinition localDef;
            std::string localUri;

            // Class member
            std::string declaringClass;
            std::vector<std::string> relatedClasses;

            // Namespace symbol
            std::string declaringNamespace;
        };

        /**
         * @brief Collects all unique indexed file URIs known to the symbol table or scope index.
         */
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
    }

    std::optional<ReferencesResult> GetReferences(const ReferencesRequest &request)
    {
        TSNode node{};
        std::string nodeText = GetNodeTextAt(request.sourceCode, request.tree, request.position, node);
        if (nodeText.empty() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        if (analysis::IsReservedKeyword(nodeText) || analysis::IsPrimitiveTypeName(nodeText))
        {
            return std::nullopt;
        }

        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        TSNode parent = ts_node_parent(node);

        TargetDescriptor target;
        target.name = nodeText;

        // 1. Context A: Member child of member_expression (obj.member)
        bool isExplicitMemberAccess = false;
        if (!ts_node_is_null(parent) && std::string_view(ts_node_type(parent)) == "member_expression")
        {
            TSNode memNode = ts_node_child_by_field_name(parent, "member", 6);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, node) || ts_node_start_byte(memNode) == ts_node_start_byte(node)))
            {
                isExplicitMemberAccess = true;
                TSNode objectNode = ts_node_child_by_field_name(parent, "object", 6);
                if (!ts_node_is_null(objectNode))
                {
                    uint32_t objStart = ts_node_start_byte(objectNode);
                    uint32_t objEnd = ts_node_end_byte(objectNode);
                    if (objStart < request.sourceCode.size() && objEnd <= request.sourceCode.size())
                    {
                        std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
                        std::string receiverTypeName;

                        if (objText == "this")
                        {
                            receiverTypeName = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
                        }
                        else if (rootScope)
                        {
                            const analysis::Scope *scope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
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
                            auto globSyms = request.symbolTable.FindSymbols(objText);
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
                            target.relatedClasses = analysis::GetAllRelatedClasses(receiverTypeName, request.symbolTable);
                        }
                    }
                }
            }
        }

        // 2. Context B: Lexical Scope Definition or Local Variable Reference (in function scope)
        if (!isExplicitMemberAccess && rootScope)
        {
            const analysis::Scope *innerScope = FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
            if (innerScope)
            {
                // Check if cursor is directly on a LocalDefinition declaration site
                const analysis::LocalDefinition *matchedDef = nullptr;
                const analysis::Scope *declScope = nullptr;

                for (const analysis::Scope *cur = innerScope; cur != nullptr; cur = cur->parent)
                {
                    for (const auto &d : cur->definitions)
                    {
                        if (d.name == nodeText &&
                            request.position.line >= d.startLine && request.position.line <= d.endLine)
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
                            target.localUri = request.uri;
                        }
                    }
                    else if (matchedDef->kind == analysis::LocalDefinitionKind::Field ||
                             matchedDef->kind == analysis::LocalDefinitionKind::Method)
                    {
                        std::string enclosingClass = GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
                        if (!enclosingClass.empty())
                        {
                            target.kind = TargetKind::ClassMember;
                            target.declaringClass = enclosingClass;
                            target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
                        }
                    }
                }
            }
        }

        // 3. Context C: Enclosing Container Search (Classes, Interfaces, Namespaces)
        if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
        {
            auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
            for (const auto &container : containers)
            {
                if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
                {
                    auto hierarchy = analysis::GetAllRelatedClasses(container.qualifiedName, request.symbolTable);
                    for (const auto &cls : hierarchy)
                    {
                        if (request.symbolTable.HasSymbol(cls + "::" + nodeText))
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
                    if (request.symbolTable.HasSymbol(qName))
                    {
                        target.kind = TargetKind::NamespaceSymbol;
                        target.declaringNamespace = container.qualifiedName;
                        target.qualifiedName = qName;
                        break;
                    }
                }
            }
        }

        // 4. Context D: SymbolTable Symbol Lookup
        if (target.kind == TargetKind::GlobalSymbol)
        {
            request.symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri == request.uri &&
                            request.position.line >= sym.startLine && request.position.line <= sym.endLine &&
                            sym.name == nodeText)
                        {
                            if (!sym.containerName.empty())
                            {
                                auto containerSyms = request.symbolTable.FindSymbols(sym.containerName);
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
                                    target.relatedClasses = analysis::GetAllRelatedClasses(sym.containerName, request.symbolTable);
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

        // Collect references based on classified target
        std::vector<lsp::Location> results;
        std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;
        std::set<std::tuple<std::string, uint32_t, uint32_t>> declRanges;

        if (target.kind == TargetKind::Local)
        {
            if (request.includeDeclaration)
            {
                results.push_back(lsp::Location{
                    lsp::DocumentUri::parse(target.localUri),
                    lsp::Range{
                        lsp::Position{ target.localDef.startLine, target.localDef.startCharacter },
                        lsp::Position{ target.localDef.endLine, target.localDef.endCharacter }
                    }
                });
                seen.insert({ target.localUri, target.localDef.startLine, target.localDef.startCharacter });
            }
            declRanges.insert({ target.localUri, target.localDef.startLine, target.localDef.startCharacter });

            std::function<void(const analysis::Scope *, bool)> collectLocal =
                [&](const analysis::Scope *scope, bool isRoot)
                {
                    if (!scope)
                    {
                        return;
                    }

                    // If a child scope declares a new definition with the exact same name, prune it (shadowing protection)
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
                            if (declRanges.contains({ target.localUri, ref.startLine, ref.startCharacter }))
                            {
                                if (!request.includeDeclaration)
                                {
                                    continue;
                                }
                            }

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

            // 1. Collect declarations from SymbolTable across related classes
            for (const auto &clsName : relatedSet)
            {
                std::string qualifiedName = clsName + "::" + target.name;
                auto syms = request.symbolTable.FindSymbols(qualifiedName);
                for (const auto &sym : syms)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                        uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                        uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                        uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                        declRanges.insert({ sym.fileUri, sL, sC });
                        if (request.includeDeclaration)
                        {
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

            // 2. Scan all indexed documents
            auto allUris = GetAllIndexedFileUris(request.symbolTable, request.uri);
            for (const auto &fileUri : allUris)
            {
                auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
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

                            if (declRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                if (!request.includeDeclaration)
                                {
                                    continue;
                                }
                            }

                            bool isMatch = false;

                            if (ref.isMemberAccess)
                            {
                                // If this is the current file with active AST
                                if (fileUri == request.uri && request.tree)
                                {
                                    TSNode rootNode = ts_tree_root_node(request.tree);
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
                                                if (oStart < request.sourceCode.size() && oEnd <= request.sourceCode.size())
                                                {
                                                    std::string oText = request.sourceCode.substr(oStart, oEnd - oStart);
                                                    std::string rType;
                                                    if (oText == "this")
                                                    {
                                                        rType = GetEnclosingClassName(request.symbolTable, fileUri, ref.startLine);
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
                                                            auto gSyms = request.symbolTable.FindSymbols(oText);
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
                                    // Cross-file member reference fallback: match if not in unrelated context
                                    isMatch = true;
                                }
                            }
                            else
                            {
                                // Implicit member access inside class method: check enclosing class and not shadowed by local var
                                std::string encClass = GetEnclosingClassName(request.symbolTable, fileUri, ref.startLine);
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
            auto syms = request.symbolTable.FindSymbols(target.qualifiedName);
            for (const auto &sym : syms)
            {
                if (sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    declRanges.insert({ sym.fileUri, sL, sC });
                    if (request.includeDeclaration)
                    {
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

            // 2. Scan all indexed documents
            auto allUris = GetAllIndexedFileUris(request.symbolTable, request.uri);
            for (const auto &fileUri : allUris)
            {
                auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
                if (!docScopeRoot)
                {
                    continue;
                }

                // Find all namespace regions in fileUri matching declaringNamespace
                std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
                request.symbolTable.ForEachSymbol(
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

                            if (declRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                if (!request.includeDeclaration)
                                {
                                    continue;
                                }
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

                            if (!isInsideNamespace && fileUri == request.uri && request.tree)
                            {
                                TSNode rootNode = ts_tree_root_node(request.tree);
                                TSPoint pt = { ref.startLine, ref.startCharacter };
                                TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                if (!ts_node_is_null(refNode))
                                {
                                    TSNode pNode = ts_node_parent(refNode);
                                    if (!ts_node_is_null(pNode) && std::string_view(ts_node_type(pNode)) == "scoped_identifier")
                                    {
                                        uint32_t pStart = ts_node_start_byte(pNode);
                                        uint32_t pEnd = ts_node_end_byte(pNode);
                                        if (pStart < request.sourceCode.size() && pEnd <= request.sourceCode.size())
                                        {
                                            std::string scoped = request.sourceCode.substr(pStart, pEnd - pStart);
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
            // 1. Collect declarations
            auto syms = request.symbolTable.FindSymbols(target.name);
            for (const auto &sym : syms)
            {
                if (sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    declRanges.insert({ sym.fileUri, sL, sC });
                    if (request.includeDeclaration)
                    {
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

            // 2. Scan all indexed documents
            auto allUris = GetAllIndexedFileUris(request.symbolTable, request.uri);
            for (const auto &fileUri : allUris)
            {
                auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
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

                            if (declRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                if (!request.includeDeclaration)
                                {
                                    continue;
                                }
                            }

                            // Verify that reference is NOT shadowed by a local variable in scope
                            const analysis::LocalDefinition *localDef = analysis::ResolveInScope(s, target.name);
                            if (localDef)
                            {
                                if (localDef->kind == analysis::LocalDefinitionKind::Parameter ||
                                    localDef->kind == analysis::LocalDefinitionKind::Variable)
                                {
                                    // If local definition is inside a function scope, the global symbol is shadowed
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

        if (results.empty())
        {
            return std::nullopt;
        }

        std::sort(results.begin(), results.end(),
                  [](const lsp::Location &a, const lsp::Location &b)
                  {
                      if (a.uri.toString() != b.uri.toString())
                      {
                          return a.uri.toString() < b.uri.toString();
                      }
                      if (a.range.start.line != b.range.start.line)
                      {
                          return a.range.start.line < b.range.start.line;
                      }
                      return a.range.start.character < b.range.start.character;
                  });

        return results;
    }
}
