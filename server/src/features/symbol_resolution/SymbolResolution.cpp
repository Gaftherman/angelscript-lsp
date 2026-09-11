#include "features/symbol_resolution/SymbolResolution.h"

#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include <spdlog/fmt/fmt.h>

// Moved here verbatim from RenameHandler.cpp, where find-references had a second copy of
// the same ~780 lines. See SymbolResolution.h for why that mattered and what pins it.
namespace angel_lsp::features::resolution
{
    namespace
    {

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
            symbolTable.ForEachSymbolInFile(
                uri,
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
     * @brief Resolves target symbol under cursor for renaming/references.
     */
    std::optional<TargetDescriptor> ResolveTargetSymbol(
        const std::string &uri,
        const std::string &sourceCode,
        TSTree *tree,
        lsp::Position position,
        const analysis::SymbolTable &symbolTable,
        const analysis::ScopeIndex &scopeIndex,
        TSNode &outNode,
        angel_lsp::utils::LspLogger *logger)
    {
        if (logger && logger->IsDebugEnabled())
        {
            logger->LogDebug(fmt::format("[SymbolResolution] ResolveTargetSymbol at {}:{} in URI: {}",
                position.line, position.character, uri));
        }

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
            TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
            if (!ts_node_is_null(memNode) && (ts_node_eq(memNode, outNode) || ts_node_start_byte(memNode) == ts_node_start_byte(outNode)))
            {
                isExplicitMemberAccess = true;
                TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
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
                    auto hierarchy = analysis::GetInheritedTypeHierarchy(container.qualifiedName, symbolTable);
                    for (const auto &cls : hierarchy)
                    {
                        if (symbolTable.HasSymbol(cls + "::" + nodeText))
                        {
                            target.kind = TargetKind::ClassMember;
                            target.declaringClass = cls;
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

        // Check if cursor is on a call expression and count arguments
        bool cursorOnCall = false;
        size_t callArgCount = 0;
        TSNode walk = outNode;
        TSNode walkP = ts_node_parent(outNode);
        while (!ts_node_is_null(walkP))
        {
            std::string_view wpType = ts_node_type(walkP);
            if (wpType == "call_expression")
            {
                TSNode funcChild = parser::GetChildByField(walkP, parser::fields::Function);
                if (ts_node_is_null(funcChild) && ts_node_child_count(walkP) > 0)
                {
                    funcChild = ts_node_child(walkP, 0);
                }
                if (!ts_node_is_null(funcChild) &&
                    (ts_node_eq(funcChild, walk) || ts_node_start_byte(funcChild) == ts_node_start_byte(walk)))
                {
                    cursorOnCall = true;
                    TSNode argsChild = parser::GetChildByField(walkP, parser::fields::Arguments);
                    if (ts_node_is_null(argsChild))
                    {
                        uint32_t cc = ts_node_child_count(walkP);
                        for (uint32_t ci = 0; ci < cc; ++ci)
                        {
                            TSNode c = ts_node_child(walkP, ci);
                            if (std::string_view(ts_node_type(c)) == "argument_list")
                            {
                                argsChild = c;
                                break;
                            }
                        }
                    }
                    if (!ts_node_is_null(argsChild))
                    {
                        uint32_t argChildCount = ts_node_child_count(argsChild);
                        for (uint32_t ai = 0; ai < argChildCount; ++ai)
                        {
                            TSNode ac = ts_node_child(argsChild, ai);
                            std::string_view act = ts_node_type(ac);
                            if (act == "(" || act == ")" || act == "," || act == ":" || act == "comment")
                            {
                                continue;
                            }
                            const char *fn = ts_node_field_name_for_child(argsChild, ai);
                            if (fn && std::string_view(fn) == "arg_name")
                            {
                                continue;
                            }
                            callArgCount++;
                        }
                    }
                }
                break;
            }
            else if (wpType == "member_expression" || wpType == "scoped_identifier")
            {
                walk = walkP;
                walkP = ts_node_parent(walkP);
            }
            else
            {
                break;
            }
        }

        if (target.kind == TargetKind::ClassMember)
        {
            analysis::AccessModifier resolvedAccess = analysis::AccessModifier::Public;
            bool foundAccess = false;

            // 1. Check if cursor is directly on the member declaration
            std::string qMember = target.declaringClass.empty() ? target.name : (target.declaringClass + "::" + target.name);
            auto localSyms = symbolTable.FindSymbols(qMember);
            if (localSyms.empty() && !target.declaringClass.empty())
            {
                localSyms = symbolTable.FindSymbols(target.name);
            }
            for (const auto &s : localSyms)
            {
                if (s.fileUri == uri && position.line >= s.startLine && position.line <= s.endLine)
                {
                    if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
                    {
                        resolvedAccess = s.GetFunction().modifiers.access;
                        foundAccess = true;
                        target.isFunction = true;
                        const auto &fn = s.GetFunction();
                        target.maxArgs = fn.parameters.size();
                        target.minArgs = 0;
                        for (const auto &p : fn.parameters)
                        {
                            if (p.defaultValue.empty())
                            {
                                target.minArgs++;
                            }
                        }
                        break;
                    }
                    else if (std::holds_alternative<analysis::VariableSignature>(s.signature))
                    {
                        resolvedAccess = s.GetVariable().modifiers.access;
                        foundAccess = true;
                        break;
                    }
                }
            }

            // 2. If not at declaration, look up in declaring class hierarchy
            if (!foundAccess)
            {
                auto hier = analysis::GetInheritedTypeHierarchy(target.declaringClass, symbolTable);
                std::optional<analysis::FunctionSignature> matchedFn;
                for (const auto &owner : hier)
                {
                    auto symsPtr = symbolTable.FindSymbolsPtr(owner + "::" + target.name);
                    if (!symsPtr)
                    {
                        continue;
                    }
                    for (const auto &s : *symsPtr)
                    {
                        if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
                        {
                            const auto &fn = s.GetFunction();
                            size_t maxP = fn.parameters.size();
                            size_t minP = 0;
                            for (const auto &p : fn.parameters)
                            {
                                if (p.defaultValue.empty())
                                {
                                    minP++;
                                }
                            }
                            if (cursorOnCall)
                            {
                                if (callArgCount >= minP && callArgCount <= maxP)
                                {
                                    matchedFn = fn;
                                    break;
                                }
                            }
                            else if (!matchedFn)
                            {
                                matchedFn = fn;
                            }
                        }
                        else if (std::holds_alternative<analysis::VariableSignature>(s.signature))
                        {
                            resolvedAccess = s.GetVariable().modifiers.access;
                            foundAccess = true;
                            break;
                        }
                    }
                    if (matchedFn || foundAccess)
                    {
                        break;
                    }
                }
                if (matchedFn)
                {
                    resolvedAccess = matchedFn->modifiers.access;
                    target.isFunction = true;
                    target.maxArgs = matchedFn->parameters.size();
                    target.minArgs = 0;
                    for (const auto &p : matchedFn->parameters)
                    {
                        if (p.defaultValue.empty())
                        {
                            target.minArgs++;
                        }
                    }
                }
            }

            target.access = resolvedAccess;
            target.relatedClasses = analysis::GetCompatibleMemberClasses(
                target.declaringClass, target.name, target.access, symbolTable);
        }
        else if (target.kind == TargetKind::NamespaceSymbol)
        {
            auto symsPtr = symbolTable.FindSymbolsPtr(target.qualifiedName);
            if (symsPtr)
            {
                std::optional<analysis::FunctionSignature> matchedFn;
                for (const auto &s : *symsPtr)
                {
                    if (s.fileUri == uri && position.line >= s.startLine && position.line <= s.endLine &&
                        std::holds_alternative<analysis::FunctionSignature>(s.signature))
                    {
                        matchedFn = s.GetFunction();
                        break;
                    }
                }
                if (!matchedFn)
                {
                    for (const auto &s : *symsPtr)
                    {
                        if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
                        {
                            const auto &fn = s.GetFunction();
                            size_t maxP = fn.parameters.size();
                            size_t minP = 0;
                            for (const auto &p : fn.parameters)
                            {
                                if (p.defaultValue.empty())
                                {
                                    minP++;
                                }
                            }
                            if (cursorOnCall)
                            {
                                if (callArgCount >= minP && callArgCount <= maxP)
                                {
                                    matchedFn = fn;
                                    break;
                                }
                            }
                            else if (!matchedFn)
                            {
                                matchedFn = fn;
                            }
                        }
                    }
                }
                if (matchedFn)
                {
                    target.isFunction = true;
                    target.maxArgs = matchedFn->parameters.size();
                    target.minArgs = 0;
                    for (const auto &p : matchedFn->parameters)
                    {
                        if (p.defaultValue.empty())
                        {
                            target.minArgs++;
                        }
                    }
                }
            }
        }
        else if (target.kind == TargetKind::GlobalSymbol)
        {
            auto symsPtr = symbolTable.FindSymbolsPtr(target.name);
            if (symsPtr)
            {
                std::optional<analysis::FunctionSignature> matchedFn;
                for (const auto &s : *symsPtr)
                {
                    if (s.fileUri == uri && position.line >= s.startLine && position.line <= s.endLine &&
                        std::holds_alternative<analysis::FunctionSignature>(s.signature))
                    {
                        matchedFn = s.GetFunction();
                        break;
                    }
                }
                if (!matchedFn)
                {
                    for (const auto &s : *symsPtr)
                    {
                        if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
                        {
                            const auto &fn = s.GetFunction();
                            size_t maxP = fn.parameters.size();
                            size_t minP = 0;
                            for (const auto &p : fn.parameters)
                            {
                                if (p.defaultValue.empty())
                                {
                                    minP++;
                                }
                            }
                            if (cursorOnCall)
                            {
                                if (callArgCount >= minP && callArgCount <= maxP)
                                {
                                    matchedFn = fn;
                                    break;
                                }
                            }
                            else if (!matchedFn)
                            {
                                matchedFn = fn;
                            }
                        }
                    }
                }
                if (matchedFn)
                {
                    target.isFunction = true;
                    target.maxArgs = matchedFn->parameters.size();
                    target.minArgs = 0;
                    for (const auto &p : matchedFn->parameters)
                    {
                        if (p.defaultValue.empty())
                        {
                            target.minArgs++;
                        }
                    }
                }
            }
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
        const analysis::ScopeIndex &scopeIndex,
        bool includeDeclaration,
        angel_lsp::utils::LspLogger *logger)
    {
        if (logger && logger->IsDebugEnabled())
        {
            logger->LogDebug(fmt::format("[SymbolResolution] CollectOccurrences for target '{}' (kind={}) in {}",
                target.name, static_cast<int>(target.kind), currentUri));
        }

        std::vector<lsp::Location> results;
        std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;
        std::set<std::tuple<std::string, uint32_t, uint32_t>> declRanges;

        if (target.kind == TargetKind::Local)
        {
            if (includeDeclaration)
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
                                if (!includeDeclaration)
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
            std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
            for (const auto &clsName : relatedSet)
            {
                std::string qualifiedName = clsName + "::" + target.name;
                auto syms = symbolTable.FindSymbols(qualifiedName);
                for (const auto &sym : syms)
                {
                    if (sym.type != analysis::SymbolType::CallReference)
                    {
                        uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                        uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                        uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                        uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                        allDeclRanges.insert({ sym.fileUri, sL, sC });

                        if (target.isFunction)
                        {
                            if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                            {
                                continue;
                            }
                            const auto &fn = sym.GetFunction();
                            size_t minP = 0;
                            for (const auto &p : fn.parameters)
                            {
                                if (p.defaultValue.empty())
                                {
                                    minP++;
                                }
                            }
                            size_t maxP = fn.parameters.size();
                            if (target.maxArgs < minP || target.minArgs > maxP)
                            {
                                continue;
                            }
                        }

                        declRanges.insert({ sym.fileUri, sL, sC });
                        if (includeDeclaration)
                        {
                            bool includeThisDecl = false;
                            std::string cleanCls = analysis::CleanBaseType(clsName);
                            std::string cleanDecl = analysis::CleanBaseType(target.declaringClass);
                            if (cleanCls == cleanDecl || cleanDecl.empty())
                            {
                                includeThisDecl = true;
                            }
                            else
                            {
                                auto hier = analysis::GetInheritedTypeHierarchy(cleanCls, symbolTable);
                                for (const auto &anc : hier)
                                {
                                    if (analysis::CleanBaseType(anc) == cleanDecl)
                                    {
                                        includeThisDecl = true;
                                        break;
                                    }
                                }
                            }

                            if (includeThisDecl && seen.insert({ sym.fileUri, sL, sC }).second)
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

                            if (allDeclRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                continue;
                            }

                            if (target.isFunction)
                            {
                                if (ref.isCall)
                                {
                                    if (ref.argumentCount < target.minArgs || ref.argumentCount > target.maxArgs)
                                    {
                                        continue;
                                    }
                                }
                            }

                            // Access modifier checks:
                            // Private & Protected members cannot be accessed outside relatedSet.
                            if (target.access == analysis::AccessModifier::Private ||
                                target.access == analysis::AccessModifier::Protected)
                            {
                                std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                if (encClass.empty() || !relatedSet.contains(encClass))
                                {
                                    continue;
                                }
                            }

                            bool isMatch = false;

                            if (ref.isMemberAccess)
                            {
                                std::string rType;
                                // If this is the current file with active AST
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
                                            TSNode objNode = parser::GetChildByField(exprParent, parser::fields::Object);
                                            if (!ts_node_is_null(objNode))
                                            {
                                                uint32_t oStart = ts_node_start_byte(objNode);
                                                uint32_t oEnd = ts_node_end_byte(objNode);
                                                if (oStart < sourceCode.size() && oEnd <= sourceCode.size())
                                                {
                                                    std::string oText = sourceCode.substr(oStart, oEnd - oStart);
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
                                                                else if (gs.type == analysis::SymbolType::Function && !gs.GetFunction().returnType.empty())
                                                                {
                                                                    rType = analysis::CleanBaseType(gs.GetFunction().returnType);
                                                                    break;
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                if (rType.empty())
                                {
                                    for (const auto &candRef : s->references)
                                    {
                                        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
                                        {
                                            if (candRef.name == "this")
                                            {
                                                rType = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                            }
                                            else
                                            {
                                                const analysis::LocalDefinition *oDef = analysis::ResolveInScope(s, candRef.name);
                                                if (oDef && !oDef->typeName.empty())
                                                {
                                                    rType = analysis::CleanBaseType(oDef->typeName);
                                                }
                                                else
                                                {
                                                    auto gSyms = symbolTable.FindSymbols(candRef.name);
                                                    for (const auto &gs : gSyms)
                                                    {
                                                        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                        {
                                                            rType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                            break;
                                                        }
                                                        else if (gs.type == analysis::SymbolType::Function && !gs.GetFunction().returnType.empty())
                                                        {
                                                            rType = analysis::CleanBaseType(gs.GetFunction().returnType);
                                                            break;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                if (!rType.empty())
                                {
                                    if (relatedSet.contains(rType))
                                    {
                                        isMatch = true;
                                    }
                                }
                                else if (fileUri != currentUri)
                                {
                                    std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                    if (encClass.empty() || relatedSet.contains(encClass))
                                    {
                                        isMatch = true;
                                    }
                                }
                            }
                            else
                            {
                                // Implicit member access inside class method: check enclosing class and not shadowed by local var
                                std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                if (!encClass.empty() && relatedSet.contains(encClass))
                                {
                                    bool inTargetHierarchy = false;
                                    std::string cleanEnc = analysis::CleanBaseType(encClass);
                                    std::string cleanDecl = analysis::CleanBaseType(target.declaringClass);
                                    if (cleanEnc == cleanDecl)
                                    {
                                        inTargetHierarchy = true;
                                    }
                                    else if (!cleanDecl.empty())
                                    {
                                        auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, symbolTable);
                                        for (const auto &ancestor : hierarchy)
                                        {
                                            if (analysis::CleanBaseType(ancestor) == cleanDecl)
                                            {
                                                inTargetHierarchy = true;
                                                break;
                                            }
                                        }
                                    }
                                    else
                                    {
                                        inTargetHierarchy = true;
                                    }

                                    if (inTargetHierarchy)
                                    {
                                        const analysis::LocalDefinition *localShadow = analysis::ResolveInScope(s, target.name);
                                        if (!localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field || localShadow->kind == analysis::LocalDefinitionKind::Method)
                                        {
                                            isMatch = true;
                                        }
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
            std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
            auto syms = symbolTable.FindSymbols(target.qualifiedName);
            for (const auto &sym : syms)
            {
                if (sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    allDeclRanges.insert({ sym.fileUri, sL, sC });

                    if (target.isFunction)
                    {
                        if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                        {
                            continue;
                        }
                        const auto &fn = sym.GetFunction();
                        size_t minP = 0;
                        for (const auto &p : fn.parameters)
                        {
                            if (p.defaultValue.empty())
                            {
                                minP++;
                            }
                        }
                        size_t maxP = fn.parameters.size();
                        if (target.maxArgs < minP || target.minArgs > maxP)
                        {
                            continue;
                        }
                    }

                    declRanges.insert({ sym.fileUri, sL, sC });
                    if (includeDeclaration)
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
            auto allUris = GetAllIndexedFileUris(symbolTable, currentUri);
            for (const auto &fileUri : allUris)
            {
                auto docScopeRoot = scopeIndex.GetRoot(fileUri);
                if (!docScopeRoot)
                {
                    continue;
                }

                // Find all namespace regions in fileUri matching declaringNamespace
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

                            if (allDeclRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                continue;
                            }

                            if (target.isFunction)
                            {
                                if (ref.isCall)
                                {
                                    if (ref.argumentCount < target.minArgs || ref.argumentCount > target.maxArgs)
                                    {
                                        continue;
                                    }
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
            // 1. Collect declarations
            std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
            auto syms = symbolTable.FindSymbols(target.name);
            for (const auto &sym : syms)
            {
                if (sym.type != analysis::SymbolType::CallReference)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
                    uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

                    allDeclRanges.insert({ sym.fileUri, sL, sC });

                    if (target.isFunction)
                    {
                        if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                        {
                            continue;
                        }
                        const auto &sig = std::get<analysis::FunctionSignature>(sym.signature);
                        size_t symMinArgs = 0;
                        size_t symMaxArgs = sig.parameters.size();
                        for (const auto &param : sig.parameters)
                        {
                            if (param.defaultValue.empty())
                            {
                                symMinArgs++;
                            }
                        }
                        if (target.minArgs > symMaxArgs || target.maxArgs < symMinArgs)
                        {
                            continue;
                        }
                    }

                    declRanges.insert({ sym.fileUri, sL, sC });
                    if (includeDeclaration)
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

                            if (allDeclRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                            {
                                continue;
                            }

                            if (target.isFunction)
                            {
                                if (ref.isCall)
                                {
                                    if (ref.argumentCount < target.minArgs || ref.argumentCount > target.maxArgs)
                                    {
                                        continue;
                                    }
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

        if (logger && logger->IsTraceEnabled())
        {
            logger->LogTrace(fmt::format("[SymbolResolution] Found {} occurrences for target '{}'",
                results.size(), target.name));
        }

        return results;
    }
}
