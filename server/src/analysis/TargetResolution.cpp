#include "analysis/TargetResolution.h"

#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <spdlog/fmt/fmt.h>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{

/**
 * @brief Checks if a node type represents an identifier or type token.
 * @param[in] nodeType Tree-Sitter node type.
 * @return True if valid identifier, primitive type, or scoped identifier.
 */
bool IsIdentifierOrType(std::string_view nodeType) noexcept
{
    return nodeType == "identifier" || nodeType == "primitive_type" || nodeType == "scoped_identifier";
}

/**
 * @brief Attempts to locate an identifier token immediately preceding the cursor position.
 * @param[in] rootNode AST root node.
 * @param[in] position Cursor position.
 * @return Preceding identifier node or null node.
 */
TSNode FindPrecedingIdentifierNode(TSNode rootNode, TargetPosition position)
{
    if (position.character == 0)
    {
        return {};
    }
    TSPoint prevPoint = {position.line, position.character - 1};
    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
    if (!ts_node_is_null(prevNode) && IsIdentifierOrType(ts_node_type(prevNode)))
    {
        return prevNode;
    }
    return {};
}

/**
 * @brief Resolves leaf identifier if node is a scoped identifier.
 * @param[in] node Starting node.
 * @param[in] point Cursor point coordinates.
 * @return Leaf identifier node or original node.
 */
TSNode ResolveIdentifierLeaf(TSNode node, TSPoint point)
{
    if (std::string_view(ts_node_type(node)) == "scoped_identifier")
    {
        TSNode leaf = ts_node_descendant_for_point_range(node, point, point);
        if (!ts_node_is_null(leaf) && std::string_view(ts_node_type(leaf)) == "identifier")
        {
            return leaf;
        }
    }
    return node;
}

/**
 * @brief Extracts token text and AST node under cursor with trailing-edge tolerance.
 * @param[in] sourceCode Document source text.
 * @param[in] tree Tree-sitter AST.
 * @param[in] position Cursor position.
 * @param[out] outNode Output TSNode.
 * @return Token text or empty string if not an identifier.
 */
std::string GetNodeTextAt(const std::string& sourceCode, TSTree* tree, TargetPosition position, TSNode& outNode)
{
    if (!tree || sourceCode.empty())
    {
        return "";
    }

    TSNode rootNode = ts_tree_root_node(tree);
    TSPoint point = {position.line, position.character};
    TSNode node = ts_node_descendant_for_point_range(rootNode, point, point);

    if (ts_node_is_null(node))
    {
        return "";
    }

    if (!IsIdentifierOrType(ts_node_type(node)))
    {
        node = FindPrecedingIdentifierNode(rootNode, position);
    }
    if (ts_node_is_null(node))
    {
        return "";
    }

    node = ResolveIdentifierLeaf(node, point);
    if (!IsIdentifierOrType(ts_node_type(node)))
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
 * @param[in] defScope Definition declaring scope.
 * @return True if enclosed in a function scope.
 */
bool IsDeclaredInFunctionScope(const analysis::Scope* defScope)
{
    size_t depth = 0;
    ankerl::unordered_dense::set<const analysis::Scope*> visited;
    for (const analysis::Scope* cur = defScope; cur != nullptr; cur = cur->parent)
    {
        if (++depth > analysis::kMaxScopeDepth || !visited.insert(cur).second)
        {
            break;
        }
        if (cur->isFunctionScope)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Resolves the enclosing class name for a given source position.
 * @param[in] symbolTable Symbol table to look up class definitions.
 * @param[in] uri Document file URI.
 * @param[in] line 0-based line number.
 * @return Enclosing class name or empty string if not in a class.
 */
std::string GetEnclosingClassName(const analysis::SymbolTable& symbolTable, const std::string& uri, uint32_t line)
{
    std::string enclosingClass;
    symbolTable.ForEachSymbolInFile(
        uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
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
 * @brief Collects all unique indexed file URIs from the symbol table plus the current document.
 * @param[in] symbolTable Global symbol table.
 * @param[in] currentUri Current document URI.
 * @return Unique list of file URIs.
 */
std::vector<std::string> GetAllIndexedFileUris(const analysis::SymbolTable& symbolTable, const std::string& currentUri)
{
    std::unordered_set<std::string> uriSet;
    if (!currentUri.empty())
    {
        uriSet.insert(currentUri);
    }

    symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!sym.fileUri.empty())
                {
                    uriSet.insert(sym.fileUri);
                }
            }
        });

    return std::vector<std::string>(uriSet.begin(), uriSet.end());
}

} // namespace

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

namespace
{

/**
 * @brief Call argument constraint information inferred from cursor position.
 */
struct FunctionArgConstraints
{
    bool cursorOnCall = false;
    size_t callArgCount = 0;
};

/**
 * @brief Resolves receiver type name from explicit object text.
 * @param[in] objText Object expression text.
 * @param[in] request Resolution request.
 * @param[in] rootScope Document root scope.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveExplicitReceiverType(const std::string& objText, const ResolveTargetRequest& request,
                                        const std::shared_ptr<const analysis::Scope>& rootScope)
{
    if (objText == "this")
    {
        return GetEnclosingClassName(request.symbolTable, request.uri, request.position.line);
    }
    if (rootScope)
    {
        const analysis::Scope* scope =
            FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
        if (scope)
        {
            const analysis::LocalDefinition* objDef = analysis::ResolveInScope(scope, objText);
            if (objDef && !objDef->typeName.empty())
            {
                return analysis::CleanBaseType(objDef->typeName);
            }
        }
    }
    auto globSyms = request.symbolTable.FindSymbols(objText);
    for (const auto& sym : globSyms)
    {
        if (sym.type == analysis::SymbolType::Variable && !sym.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(sym.GetVariable().typeName);
        }
    }
    return "";
}

/**
 * @brief Resolves explicit member access expression (obj.mem) under cursor.
 * @param[in] outNode Identifier AST node.
 * @param[in] request Resolution request.
 * @param[in] rootScope Document root scope.
 * @param[out] target Target descriptor to populate.
 * @return True if cursor is on an explicit member access expression.
 */
bool ResolveExplicitMemberAccess(TSNode outNode, const ResolveTargetRequest& request,
                                 const std::shared_ptr<const analysis::Scope>& rootScope, TargetDescriptor& target)
{
    TSNode parent = ts_node_parent(outNode);
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
    if (ts_node_is_null(memNode) ||
        (!ts_node_eq(memNode, outNode) && ts_node_start_byte(memNode) != ts_node_start_byte(outNode)))
    {
        return false;
    }
    TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
    if (ts_node_is_null(objectNode))
    {
        return false;
    }
    uint32_t objStart = ts_node_start_byte(objectNode);
    uint32_t objEnd = ts_node_end_byte(objectNode);
    if (objStart >= request.sourceCode.size() || objEnd > request.sourceCode.size())
    {
        return false;
    }
    std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
    std::string receiverTypeName = ResolveExplicitReceiverType(objText, request, rootScope);
    if (!receiverTypeName.empty())
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = receiverTypeName;
    }
    return true;
}

/**
 * @brief Searches for a local definition matching name and line spanning scope ancestors.
 * @param[in] innerScope Innermost scope.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] line Target line number.
 * @param[out] outDeclScope Scope where definition was found.
 * @return Pointer to definition if found, nullptr otherwise.
 */
const analysis::LocalDefinition* FindLocalDefinitionAtLine(const analysis::Scope* innerScope,
                                                           const std::string& nodeText, uint32_t line,
                                                           const analysis::Scope*& outDeclScope)
{
    size_t depth = 0;
    ankerl::unordered_dense::set<const analysis::Scope*> visited;
    for (const analysis::Scope* cur = innerScope; cur != nullptr; cur = cur->parent)
    {
        if (++depth > analysis::kMaxScopeDepth || !visited.insert(cur).second)
        {
            break;
        }
        for (const auto& d : cur->definitions)
        {
            if (d.name == nodeText && line >= d.startLine && line <= d.endLine)
            {
                outDeclScope = cur;
                return &d;
            }
        }
    }
    return nullptr;
}

/**
 * @brief Resolves target as a local variable, parameter, or enclosing class member.
 * @param[in] rootScope Document root scope.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Resolution request.
 * @param[out] target Target descriptor to populate.
 */
void ResolveLocalTarget(const std::shared_ptr<const analysis::Scope>& rootScope, const std::string& nodeText,
                        const ResolveTargetRequest& request, TargetDescriptor& target)
{
    if (!rootScope)
    {
        return;
    }
    const analysis::Scope* innerScope =
        FindInnermostScope(rootScope.get(), request.position.line, request.position.character);
    if (!innerScope)
    {
        return;
    }
    const analysis::Scope* declScope = nullptr;
    const analysis::LocalDefinition* matchedDef =
        FindLocalDefinitionAtLine(innerScope, nodeText, request.position.line, declScope);
    if (!matchedDef)
    {
        matchedDef = analysis::ResolveInScope(innerScope, nodeText);
        if (matchedDef)
        {
            declScope = analysis::FindScopeDeclaringDefinition(rootScope.get(), *matchedDef);
        }
    }
    if (!matchedDef || !declScope)
    {
        return;
    }
    if (matchedDef->kind == analysis::LocalDefinitionKind::Parameter ||
        matchedDef->kind == analysis::LocalDefinitionKind::Variable)
    {
        if (IsDeclaredInFunctionScope(declScope) || matchedDef->kind == analysis::LocalDefinitionKind::Parameter)
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
        }
    }
}

/**
 * @brief Resolves target through enclosing class, interface, or namespace containers.
 * @param[in] outNode AST node.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Resolution request.
 * @param[out] target Target descriptor to populate.
 */
void ResolveContainerTarget(TSNode outNode, const std::string& nodeText, const ResolveTargetRequest& request,
                            TargetDescriptor& target)
{
    auto containers = analysis::GetEnclosingContainers(outNode, request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetInheritedTypeHierarchy(container.qualifiedName, request.symbolTable);
            for (const auto& cls : hierarchy)
            {
                if (request.symbolTable.HasSymbol(cls + "::" + nodeText))
                {
                    target.kind = TargetKind::ClassMember;
                    target.declaringClass = cls;
                    return;
                }
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
                return;
            }
        }
    }
}

/**
 * @brief Checks global symbols for class or namespace containers matching the symbol.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Resolution request.
 * @param[out] target Target descriptor to populate.
 */
void ResolveGlobalFallbackTarget(const std::string& nodeText, const ResolveTargetRequest& request,
                                 TargetDescriptor& target)
{
    request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.fileUri == request.uri && request.position.line >= sym.startLine &&
                    request.position.line <= sym.endLine && sym.name == nodeText)
                {
                    if (sym.containerName.empty())
                    {
                        continue;
                    }
                    auto containerSyms = request.symbolTable.FindSymbols(sym.containerName);
                    for (const auto& csym : containerSyms)
                    {
                        if (csym.type == analysis::SymbolType::Class || csym.type == analysis::SymbolType::Interface)
                        {
                            target.kind = TargetKind::ClassMember;
                            target.declaringClass = sym.containerName;
                            return;
                        }
                        if (csym.type == analysis::SymbolType::Namespace)
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

/**
 * @brief Counts call arguments from an argument_list AST node.
 * @param[in] argsChild Argument list AST node.
 * @return Number of call argument expressions.
 */
size_t CountCallArguments(TSNode argsChild)
{
    size_t count = 0;
    uint32_t argChildCount = ts_node_child_count(argsChild);
    for (uint32_t ai = 0; ai < argChildCount; ++ai)
    {
        TSNode ac = ts_node_child(argsChild, ai);
        std::string_view act = ts_node_type(ac);
        if (act == "(" || act == ")" || act == "," || act == ":" || act == "comment")
        {
            continue;
        }
        const char* fn = ts_node_field_name_for_child(argsChild, ai);
        if (fn && std::string_view(fn) == "arg_name")
        {
            continue;
        }
        count++;
    }
    return count;
}

/**
 * @brief Finds argument_list node from a call_expression node.
 * @param[in] callNode Call expression AST node.
 * @return Argument list node or null node.
 */
TSNode FindArgumentListNode(TSNode callNode)
{
    TSNode argsChild = parser::GetChildByField(callNode, parser::fields::Arguments);
    if (!ts_node_is_null(argsChild))
    {
        return argsChild;
    }
    uint32_t cc = ts_node_child_count(callNode);
    for (uint32_t ci = 0; ci < cc; ++ci)
    {
        TSNode c = ts_node_child(callNode, ci);
        if (std::string_view(ts_node_type(c)) == "argument_list")
        {
            return c;
        }
    }
    return {};
}

/**
 * @brief Gathers call expression and argument constraints for identifier under cursor.
 * @param[in] outNode Identifier AST node.
 * @return Function argument constraints.
 */
FunctionArgConstraints GetCallArgumentConstraints(TSNode outNode)
{
    FunctionArgConstraints info;
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
                info.cursorOnCall = true;
                TSNode argsChild = FindArgumentListNode(walkP);
                if (!ts_node_is_null(argsChild))
                {
                    info.callArgCount = CountCallArguments(argsChild);
                }
            }
            break;
        }
        if (wpType == "member_expression" || wpType == "scoped_identifier")
        {
            walk = walkP;
            walkP = ts_node_parent(walkP);
        }
        else
        {
            break;
        }
    }
    return info;
}

/**
 * @brief Matches an appropriate function signature from candidate symbols based on call constraints.
 * @param[in] symbols Candidate symbol bucket.
 * @param[in] uri Target document URI.
 * @param[in] line Target line number.
 * @param[in] callInfo Call arguments info.
 * @return Optional matching FunctionSignature.
 */
std::optional<analysis::FunctionSignature> MatchFunctionSignature(const std::vector<analysis::Symbol>& symbols,
                                                                  const std::string& uri, uint32_t line,
                                                                  const FunctionArgConstraints& callInfo)
{
    for (const auto& s : symbols)
    {
        if (s.fileUri == uri && line >= s.startLine && line <= s.endLine &&
            std::holds_alternative<analysis::FunctionSignature>(s.signature))
        {
            return s.GetFunction();
        }
    }
    std::optional<analysis::FunctionSignature> fallback;
    for (const auto& s : symbols)
    {
        if (!std::holds_alternative<analysis::FunctionSignature>(s.signature))
        {
            continue;
        }
        const auto& fn = s.GetFunction();
        size_t minP = 0;
        for (const auto& p : fn.parameters)
        {
            if (p.defaultValue.empty())
            {
                minP++;
            }
        }
        size_t maxP = fn.parameters.size();
        if (callInfo.cursorOnCall)
        {
            if (callInfo.callArgCount >= minP && callInfo.callArgCount <= maxP)
            {
                return fn;
            }
        }
        else if (!fallback)
        {
            fallback = fn;
        }
    }
    return fallback;
}

/**
 * @brief Configures target descriptor function properties from signature.
 * @param[out] target Target descriptor to update.
 * @param[in] fn Matched function signature.
 */
void ApplyFunctionSignature(TargetDescriptor& target, const analysis::FunctionSignature& fn)
{
    target.isFunction = true;
    target.maxArgs = fn.parameters.size();
    target.minArgs = 0;
    for (const auto& p : fn.parameters)
    {
        if (p.defaultValue.empty())
        {
            target.minArgs++;
        }
    }
}

/**
 * @brief Looks up a member directly declared in the class or file.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[out] resolvedAccess Access modifier if found.
 * @return True if direct member symbol was found.
 */
bool ResolveDirectClassMember(TargetDescriptor& target, const ResolveTargetRequest& request,
                              analysis::AccessModifier& resolvedAccess)
{
    std::string qMember = target.declaringClass.empty() ? target.name : (target.declaringClass + "::" + target.name);
    auto localSyms = request.symbolTable.FindSymbols(qMember);
    if (localSyms.empty() && !target.declaringClass.empty())
    {
        localSyms = request.symbolTable.FindSymbols(target.name);
    }
    for (const auto& s : localSyms)
    {
        if (s.fileUri == request.uri && request.position.line >= s.startLine && request.position.line <= s.endLine)
        {
            if (std::holds_alternative<analysis::FunctionSignature>(s.signature))
            {
                resolvedAccess = s.GetFunction().modifiers.access;
                ApplyFunctionSignature(target, s.GetFunction());
                return true;
            }
            if (std::holds_alternative<analysis::VariableSignature>(s.signature))
            {
                resolvedAccess = s.GetVariable().modifiers.access;
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Looks up an inherited member in ancestor classes.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 * @param[out] resolvedAccess Access modifier if found.
 */
void ResolveInheritedClassMember(TargetDescriptor& target, const ResolveTargetRequest& request,
                                 const FunctionArgConstraints& callInfo, analysis::AccessModifier& resolvedAccess)
{
    auto hier = analysis::GetInheritedTypeHierarchy(target.declaringClass, request.symbolTable);
    for (const auto& owner : hier)
    {
        auto symsPtr = request.symbolTable.FindSymbolsPtr(owner + "::" + target.name);
        if (!symsPtr)
        {
            continue;
        }
        if (auto fn = MatchFunctionSignature(*symsPtr, request.uri, request.position.line, callInfo))
        {
            resolvedAccess = fn->modifiers.access;
            ApplyFunctionSignature(target, *fn);
            return;
        }
        for (const auto& s : *symsPtr)
        {
            if (std::holds_alternative<analysis::VariableSignature>(s.signature))
            {
                resolvedAccess = s.GetVariable().modifiers.access;
                return;
            }
        }
    }
}

/**
 * @brief Resolves access modifier and overload signature for a class member target.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 */
void ResolveClassMemberDetails(TargetDescriptor& target, const ResolveTargetRequest& request,
                               const FunctionArgConstraints& callInfo)
{
    analysis::AccessModifier resolvedAccess = analysis::AccessModifier::Public;
    if (!ResolveDirectClassMember(target, request, resolvedAccess))
    {
        ResolveInheritedClassMember(target, request, callInfo, resolvedAccess);
    }
    target.access = resolvedAccess;
    target.relatedClasses =
        analysis::GetCompatibleMemberClasses(target.declaringClass, target.name, target.access, request.symbolTable);
}

/**
 * @brief Resolves overload function signature for class member, namespace, or global symbol.
 * @param[in,out] target Target descriptor.
 * @param[in] request Resolution request.
 * @param[in] callInfo Call argument constraints.
 */
void ResolveTargetSignature(TargetDescriptor& target, const ResolveTargetRequest& request,
                            const FunctionArgConstraints& callInfo)
{
    if (target.kind == TargetKind::ClassMember)
    {
        ResolveClassMemberDetails(target, request, callInfo);
        return;
    }
    if (target.kind != TargetKind::NamespaceSymbol && target.kind != TargetKind::GlobalSymbol)
    {
        return;
    }
    const std::string& query = (target.kind == TargetKind::NamespaceSymbol) ? target.qualifiedName : target.name;
    if (auto symsPtr = request.symbolTable.FindSymbolsPtr(query))
    {
        if (auto fn = MatchFunctionSignature(*symsPtr, request.uri, request.position.line, callInfo))
        {
            ApplyFunctionSignature(target, *fn);
        }
    }
}

} // namespace

std::optional<TargetDescriptor> ResolveTargetSymbol(ResolveTargetRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[TargetResolution] ResolveTargetSymbol at {}:{} in URI: {}",
                                             request.position.line, request.position.character, request.uri));
    }

    std::string nodeText = GetNodeTextAt(request.sourceCode, request.tree, request.position, request.outNode);
    if (nodeText.empty() || ts_node_is_null(request.outNode) || !IsValidIdentifier(nodeText))
    {
        return std::nullopt;
    }

    auto rootScope = request.scopeIndex.GetRoot(request.uri);

    TargetDescriptor target;
    target.name = nodeText;

    bool isExplicitMemberAccess = ResolveExplicitMemberAccess(request.outNode, request, rootScope, target);
    if (!isExplicitMemberAccess)
    {
        ResolveLocalTarget(rootScope, nodeText, request, target);
    }
    if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
    {
        ResolveContainerTarget(request.outNode, nodeText, request, target);
    }
    if (target.kind == TargetKind::GlobalSymbol)
    {
        ResolveGlobalFallbackTarget(nodeText, request, target);
    }

    FunctionArgConstraints callInfo = GetCallArgumentConstraints(request.outNode);
    ResolveTargetSignature(target, request, callInfo);

    return target;
}

namespace
{

/**
 * @brief Accumulator and deduplication state for occurrence locations.
 */
struct OccurrenceCollector
{
    std::vector<OccurrenceLocation> results;
    std::set<std::tuple<std::string, uint32_t, uint32_t>> seen;
    std::set<std::tuple<std::string, uint32_t, uint32_t>> declRanges;

    /**
     * @brief Adds a location if not already collected.
     * @param[in] uri File URI.
     * @param[in] range Target range.
     */
    void Add(const std::string& uri, const SourceRange& range)
    {
        if (seen.insert({uri, range.startLine, range.startCharacter}).second)
        {
            results.push_back(OccurrenceLocation{uri, range});
        }
    }

    /**
     * @brief Adds a local reference occurrence.
     * @param[in] uri File URI.
     * @param[in] ref Reference to add.
     */
    void Add(const std::string& uri, const analysis::LocalReference& ref)
    {
        Add(uri, SourceRange{ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter});
    }
};

/**
 * @brief Start and end coordinates for a symbol.
 */
struct SymbolSpan
{
    uint32_t sL = 0;
    uint32_t sC = 0;
    uint32_t eL = 0;
    uint32_t eC = 0;
};

/**
 * @brief Context for scanning documents during occurrence collection.
 */
struct OccurrenceScanContext
{
    const std::string& fileUri;
    const CollectOccurrencesRequest& request;
    const std::set<std::tuple<std::string, uint32_t, uint32_t>>& declRanges;
    OccurrenceCollector& collector;
};

/**
 * @brief Extracts effective selection or full range span for a symbol.
 * @param[in] sym Target symbol.
 * @return Coordinate span.
 */
SymbolSpan GetSymbolSpan(const analysis::Symbol& sym) noexcept
{
    bool hasSel = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0);
    return SymbolSpan{
        .sL = hasSel ? sym.selectionRange.startLine : sym.startLine,
        .sC = hasSel ? sym.selectionRange.startCharacter : sym.startCharacter,
        .eL = hasSel ? sym.selectionRange.endLine : sym.endLine,
        .eC = hasSel ? sym.selectionRange.endCharacter : sym.endCharacter,
    };
}

/**
 * @brief Checks if a function symbol's parameter count matches the target function requirements.
 * @param[in] sym Symbol to test.
 * @param[in] target Target descriptor.
 * @return True if function parameters are compatible.
 */
bool FunctionMatchesTarget(const analysis::Symbol& sym, const TargetDescriptor& target)
{
    if (!target.isFunction)
    {
        return true;
    }
    if (!std::holds_alternative<analysis::FunctionSignature>(sym.signature))
    {
        return false;
    }
    const auto& fn = sym.GetFunction();
    size_t minP = 0;
    for (const auto& p : fn.parameters)
    {
        if (p.defaultValue.empty())
        {
            minP++;
        }
    }
    size_t maxP = fn.parameters.size();
    return target.maxArgs >= minP && target.minArgs <= maxP;
}

/**
 * @brief Collects all occurrences for a local variable or parameter target.
 * @param[in] target Target descriptor.
 * @param[in] includeDeclaration True to include declaration site.
 * @param[in,out] collector Occurrence accumulator.
 */
void CollectLocalOccurrences(const TargetDescriptor& target, bool includeDeclaration, OccurrenceCollector& collector)
{
    if (includeDeclaration)
    {
        collector.Add(target.localUri, SourceRange{target.localDef.startLine, target.localDef.startCharacter,
                                                   target.localDef.endLine, target.localDef.endCharacter});
    }
    collector.declRanges.insert({target.localUri, target.localDef.startLine, target.localDef.startCharacter});

    if (!target.definingScope)
    {
        return;
    }

    std::vector<std::pair<const analysis::Scope*, bool>> stack{{target.definingScope, true}};
    while (!stack.empty())
    {
        auto [scope, isRoot] = stack.back();
        stack.pop_back();

        if (!isRoot)
        {
            bool shadowed = false;
            for (const auto& def : scope->definitions)
            {
                if (def.name == target.name)
                {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed)
            {
                continue;
            }
        }

        for (const auto& ref : scope->references)
        {
            if (ref.name == target.name && !ref.isMemberAccess)
            {
                if (!includeDeclaration &&
                    collector.declRanges.contains({target.localUri, ref.startLine, ref.startCharacter}))
                {
                    continue;
                }
                collector.Add(target.localUri, ref);
            }
        }

        for (const auto& child : scope->children)
        {
            if (child)
            {
                stack.push_back({child.get(), false});
            }
        }
    }
}

/**
 * @brief Resolves type name for an identifier from scope or global symbol table.
 * @param[in] name Identifier name.
 * @param[in] scope Lexical scope.
 * @param[in] request Collect occurrences request.
 * @param[in] line Source line number.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveNamedIdentifierType(const std::string& name, const analysis::Scope* scope,
                                       const CollectOccurrencesRequest& request, uint32_t line)
{
    if (name == "this")
    {
        return GetEnclosingClassName(request.symbolTable, request.currentUri, line);
    }
    const analysis::LocalDefinition* oDef = analysis::ResolveInScope(scope, name);
    if (oDef && !oDef->typeName.empty())
    {
        return analysis::CleanBaseType(oDef->typeName);
    }
    auto gSyms = request.symbolTable.FindSymbols(name);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(gs.GetVariable().typeName);
        }
        if (gs.type == analysis::SymbolType::Function && !gs.GetFunction().returnType.empty())
        {
            return analysis::CleanBaseType(gs.GetFunction().returnType);
        }
    }
    return "";
}

/**
 * @brief Resolves receiver type name from AST member expression.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] ctx Occurrence scan context.
 * @return Cleaned receiver type or empty string.
 */
std::string ResolveReceiverFromAst(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                   const OccurrenceScanContext& ctx)
{
    if (ctx.fileUri != ctx.request.currentUri || !ctx.request.tree)
    {
        return "";
    }
    TSNode rootNode = ts_tree_root_node(ctx.request.tree);
    TSPoint pt = {ref.startLine, ref.startCharacter};
    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(refNode))
    {
        return "";
    }
    TSNode exprParent = ts_node_parent(refNode);
    if (ts_node_is_null(exprParent) || std::string_view(ts_node_type(exprParent)) != "member_expression")
    {
        return "";
    }
    TSNode objNode = parser::GetChildByField(exprParent, parser::fields::Object);
    if (ts_node_is_null(objNode))
    {
        return "";
    }
    uint32_t oStart = ts_node_start_byte(objNode);
    uint32_t oEnd = ts_node_end_byte(objNode);
    if (oStart >= ctx.request.sourceCode.size() || oEnd > ctx.request.sourceCode.size())
    {
        return "";
    }
    std::string oText = ctx.request.sourceCode.substr(oStart, oEnd - oStart);
    return ResolveNamedIdentifierType(oText, scope, ctx.request, ref.startLine);
}

/**
 * @brief Resolves receiver type name by scanning preceding references in scope.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] ctx Occurrence scan context.
 * @return Cleaned receiver type or empty string.
 */
std::string ResolveReceiverFromScope(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                     const OccurrenceScanContext& ctx)
{
    for (const auto& candRef : scope->references)
    {
        if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
        {
            std::string t = ResolveNamedIdentifierType(candRef.name, scope, ctx.request, ref.startLine);
            if (!t.empty())
            {
                return t;
            }
        }
    }
    return "";
}

/**
 * @brief Resolves receiver type name from AST or scope for member expression reference.
 * @param[in] ref Reference under test.
 * @param[in] scope Lexical scope.
 * @param[in] ctx Occurrence scan context.
 * @return Cleaned receiver type or empty string.
 */
std::string ResolveReceiverType(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                const OccurrenceScanContext& ctx)
{
    std::string astType = ResolveReceiverFromAst(ref, scope, ctx);
    if (!astType.empty())
    {
        return astType;
    }
    return ResolveReceiverFromScope(ref, scope, ctx);
}

/**
 * @brief Checks whether an implicit member access matches target class hierarchy and is not locally shadowed.
 * @param[in] ref Reference to test.
 * @param[in] scope Lexical scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in] ctx Occurrence scan context.
 * @return True if implicit member access is a valid match.
 */
bool CheckImplicitMemberAccess(const analysis::LocalReference& ref, const analysis::Scope* scope,
                               const std::unordered_set<std::string>& relatedSet, const OccurrenceScanContext& ctx)
{
    std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
    if (encClass.empty() || !relatedSet.contains(encClass))
    {
        return false;
    }

    bool inTargetHierarchy = false;
    std::string cleanEnc = analysis::CleanBaseType(encClass);
    std::string cleanDecl = analysis::CleanBaseType(ctx.request.target.declaringClass);
    if (cleanEnc == cleanDecl || cleanDecl.empty())
    {
        inTargetHierarchy = true;
    }
    else
    {
        auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, ctx.request.symbolTable);
        for (const auto& ancestor : hierarchy)
        {
            if (analysis::CleanBaseType(ancestor) == cleanDecl)
            {
                inTargetHierarchy = true;
                break;
            }
        }
    }

    if (!inTargetHierarchy)
    {
        return false;
    }

    const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(scope, ctx.request.target.name);
    return !localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field ||
           localShadow->kind == analysis::LocalDefinitionKind::Method;
}

/**
 * @brief Determines if a class member declaration should be included in occurrences.
 * @param[in] clsName Class name where symbol is declared.
 * @param[in] declaringClass Target's declaring class name.
 * @param[in] symbolTable Global symbol table.
 * @return True if declaration belongs to the target's hierarchy.
 */
bool ShouldIncludeClassDecl(const std::string& clsName, const std::string& declaringClass,
                            const analysis::SymbolTable& symbolTable)
{
    std::string cleanCls = analysis::CleanBaseType(clsName);
    std::string cleanDecl = analysis::CleanBaseType(declaringClass);
    if (cleanCls == cleanDecl || cleanDecl.empty())
    {
        return true;
    }
    auto hier = analysis::GetInheritedTypeHierarchy(cleanCls, symbolTable);
    for (const auto& anc : hier)
    {
        if (analysis::CleanBaseType(anc) == cleanDecl)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Collects declarations of class members across related classes.
 * @param[in] relatedSet Set of related class names.
 * @param[in] request Occurrences request.
 * @param[out] allDeclRanges Set of all declaration coordinate tuples.
 * @param[in,out] collector Occurrence accumulator.
 */
void CollectClassMemberDeclarations(const std::unordered_set<std::string>& relatedSet,
                                    const CollectOccurrencesRequest& request,
                                    std::set<std::tuple<std::string, uint32_t, uint32_t>>& allDeclRanges,
                                    OccurrenceCollector& collector)
{
    for (const auto& clsName : relatedSet)
    {
        std::string qualifiedName = clsName + "::" + request.target.name;
        auto syms = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : syms)
        {
            if (sym.type == analysis::SymbolType::CallReference)
            {
                continue;
            }
            SymbolSpan span = GetSymbolSpan(sym);
            allDeclRanges.insert({sym.fileUri, span.sL, span.sC});

            if (!FunctionMatchesTarget(sym, request.target))
            {
                continue;
            }

            collector.declRanges.insert({sym.fileUri, span.sL, span.sC});
            if (request.includeDeclaration &&
                ShouldIncludeClassDecl(clsName, request.target.declaringClass, request.symbolTable))
            {
                collector.Add(sym.fileUri, SourceRange{span.sL, span.sC, span.eL, span.eC});
            }
        }
    }
}

/**
 * @brief Checks if a call reference satisfies target argument count constraints.
 * @param[in] ref Reference to check.
 * @param[in] target Target descriptor.
 * @return True if argument count matches.
 */
bool MatchesCallArguments(const analysis::LocalReference& ref, const TargetDescriptor& target)
{
    if (target.isFunction && ref.isCall)
    {
        return ref.argumentCount >= target.minArgs && ref.argumentCount <= target.maxArgs;
    }
    return true;
}

/**
 * @brief Evaluates whether a class member reference matches the target symbol.
 * @param[in] ref Reference to test.
 * @param[in] scope Lexical scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in] ctx Occurrence scan context.
 * @return True if reference is a match.
 */
bool IsClassMemberReferenceMatch(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                 const std::unordered_set<std::string>& relatedSet, const OccurrenceScanContext& ctx)
{
    if (ref.isMemberAccess)
    {
        std::string rType = ResolveReceiverType(ref, scope, ctx);
        if (!rType.empty())
        {
            return relatedSet.contains(rType);
        }
        if (ctx.fileUri != ctx.request.currentUri)
        {
            std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
            return encClass.empty() || relatedSet.contains(encClass);
        }
        return false;
    }
    return CheckImplicitMemberAccess(ref, scope, relatedSet, ctx);
}

/**
 * @brief Scans a document scope tree for class member occurrences.
 * @param[in] root Document root scope.
 * @param[in] relatedSet Set of related class names.
 * @param[in,out] ctx Occurrence scan context.
 */
void ScanDocumentForClassMember(const analysis::Scope* root, const std::unordered_set<std::string>& relatedSet,
                                OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name != ctx.request.target.name ||
                ctx.declRanges.contains({ctx.fileUri, ref.startLine, ref.startCharacter}))
            {
                continue;
            }
            if (!MatchesCallArguments(ref, ctx.request.target))
            {
                continue;
            }
            if (ctx.request.target.access == analysis::AccessModifier::Private ||
                ctx.request.target.access == analysis::AccessModifier::Protected)
            {
                std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.fileUri, ref.startLine);
                if (encClass.empty() || !relatedSet.contains(encClass))
                {
                    continue;
                }
            }
            if (IsClassMemberReferenceMatch(ref, s, relatedSet, ctx))
            {
                ctx.collector.Add(ctx.fileUri, ref);
            }
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects all occurrences of a class member across all indexed documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Occurrence accumulator.
 */
void CollectClassMemberOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::unordered_set<std::string> relatedSet(request.target.relatedClasses.begin(),
                                               request.target.relatedClasses.end());
    if (!request.target.declaringClass.empty())
    {
        relatedSet.insert(request.target.declaringClass);
    }

    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectClassMemberDeclarations(relatedSet, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
        if (!docScopeRoot)
        {
            continue;
        }
        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForClassMember(docScopeRoot.get(), relatedSet, ctx);
    }
}

/**
 * @brief Checks whether a local definition in scope shadows a global or namespace symbol.
 * @param[in] scope Current lexical scope.
 * @param[in] name Symbol identifier name.
 * @param[in] root Root scope of document.
 * @return True if shadowed by a function-local variable or parameter.
 */
bool IsShadowedInFunctionScope(const analysis::Scope* scope, const std::string& name, const analysis::Scope* root)
{
    const analysis::LocalDefinition* localDef = analysis::ResolveInScope(scope, name);
    if (!localDef)
    {
        return false;
    }
    if (localDef->kind != analysis::LocalDefinitionKind::Parameter &&
        localDef->kind != analysis::LocalDefinitionKind::Variable)
    {
        return false;
    }
    const analysis::Scope* defScope = analysis::FindScopeDeclaringDefinition(root, *localDef);
    return defScope && IsDeclaredInFunctionScope(defScope);
}

/**
 * @brief Collects declarations of a namespace or global symbol.
 * @param[in] symQuery Symbol name or qualified name to query.
 * @param[in] request Occurrences request.
 * @param[out] allDeclRanges Declaration coordinate tuples.
 * @param[in,out] collector Accumulator.
 */
void CollectDeclarations(const std::string& symQuery, const CollectOccurrencesRequest& request,
                         std::set<std::tuple<std::string, uint32_t, uint32_t>>& allDeclRanges,
                         OccurrenceCollector& collector)
{
    auto syms = request.symbolTable.FindSymbols(symQuery);
    for (const auto& sym : syms)
    {
        if (sym.type == analysis::SymbolType::CallReference)
        {
            continue;
        }
        SymbolSpan span = GetSymbolSpan(sym);
        allDeclRanges.insert({sym.fileUri, span.sL, span.sC});

        if (!FunctionMatchesTarget(sym, request.target))
        {
            continue;
        }

        collector.declRanges.insert({sym.fileUri, span.sL, span.sC});
        if (request.includeDeclaration)
        {
            collector.Add(sym.fileUri, SourceRange{span.sL, span.sC, span.eL, span.eC});
        }
    }
}

/**
 * @brief Checks whether a reference is enclosed within a namespace region or explicitly scoped.
 * @param[in] fileUri Document URI.
 * @param[in] ref Reference to check.
 * @param[in] nsRanges Line ranges of declaring namespace.
 * @param[in] request Occurrences request.
 * @return True if reference is inside declaring namespace.
 */
bool IsRefInsideNamespace(const std::string& fileUri, const analysis::LocalReference& ref,
                          const std::vector<std::pair<uint32_t, uint32_t>>& nsRanges,
                          const CollectOccurrencesRequest& request)
{
    for (const auto& nr : nsRanges)
    {
        if (ref.startLine >= nr.first && ref.startLine <= nr.second)
        {
            return true;
        }
    }

    if (fileUri == request.currentUri && request.tree)
    {
        TSNode rootNode = ts_tree_root_node(request.tree);
        TSPoint pt = {ref.startLine, ref.startCharacter};
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
                    if (scoped == request.target.qualifiedName)
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/**
 * @brief Scans a document scope tree for namespace symbol references.
 * @param[in] root Document root scope.
 * @param[in] nsRanges Namespace line spans in fileUri.
 * @param[in,out] ctx Occurrence scan context.
 */
void ScanDocumentForNamespace(const analysis::Scope* root, const std::vector<std::pair<uint32_t, uint32_t>>& nsRanges,
                              OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name != ctx.request.target.name || ref.isMemberAccess ||
                ctx.declRanges.contains({ctx.fileUri, ref.startLine, ref.startCharacter}))
            {
                continue;
            }
            if (!MatchesCallArguments(ref, ctx.request.target))
            {
                continue;
            }
            if (!IsRefInsideNamespace(ctx.fileUri, ref, nsRanges, ctx.request))
            {
                continue;
            }
            if (IsShadowedInFunctionScope(s, ctx.request.target.name, root))
            {
                continue;
            }
            ctx.collector.Add(ctx.fileUri, ref);
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects occurrences of a namespace-scoped symbol across documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Accumulator.
 */
void CollectNamespaceOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectDeclarations(request.target.qualifiedName, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
        if (!docScopeRoot)
        {
            continue;
        }

        std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
        request.symbolTable.ForEachSymbol(
            [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& sList)
            {
                for (const auto& s : sList)
                {
                    if (s.type == analysis::SymbolType::Namespace && s.fileUri == fileUri &&
                        (s.name == request.target.declaringNamespace ||
                         s.qualifiedName == request.target.declaringNamespace))
                    {
                        nsRanges.push_back({s.startLine, s.endLine});
                    }
                }
            });

        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForNamespace(docScopeRoot.get(), nsRanges, ctx);
    }
}

/**
 * @brief Scans a document scope tree for global symbol references.
 * @param[in] root Document root scope.
 * @param[in,out] ctx Occurrence scan context.
 */
void ScanDocumentForGlobal(const analysis::Scope* root, OccurrenceScanContext& ctx)
{
    if (!root)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{root};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name != ctx.request.target.name || ref.isMemberAccess ||
                ctx.declRanges.contains({ctx.fileUri, ref.startLine, ref.startCharacter}))
            {
                continue;
            }
            if (!MatchesCallArguments(ref, ctx.request.target))
            {
                continue;
            }
            if (IsShadowedInFunctionScope(s, ctx.request.target.name, root))
            {
                continue;
            }
            ctx.collector.Add(ctx.fileUri, ref);
        }

        for (const auto& child : s->children)
        {
            if (child)
            {
                stack.push_back(child.get());
            }
        }
    }
}

/**
 * @brief Collects occurrences of a global symbol across documents.
 * @param[in] request Occurrences request.
 * @param[in,out] collector Accumulator.
 */
void CollectGlobalOccurrences(const CollectOccurrencesRequest& request, OccurrenceCollector& collector)
{
    std::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
    CollectDeclarations(request.target.name, request, allDeclRanges, collector);

    auto allUris = GetAllIndexedFileUris(request.symbolTable, request.currentUri);
    for (const auto& fileUri : allUris)
    {
        auto docScopeRoot = request.scopeIndex.GetRoot(fileUri);
        if (!docScopeRoot)
        {
            continue;
        }
        OccurrenceScanContext ctx{
            .fileUri = fileUri,
            .request = request,
            .declRanges = allDeclRanges,
            .collector = collector,
        };
        ScanDocumentForGlobal(docScopeRoot.get(), ctx);
    }
}

} // namespace

std::vector<OccurrenceLocation> CollectOccurrences(const CollectOccurrencesRequest& request)
{
    if (request.logger && request.logger->IsDebugEnabled())
    {
        request.logger->LogDebug(fmt::format("[TargetResolution] CollectOccurrences for target '{}' (kind={}) in {}",
                                             request.target.name, static_cast<int>(request.target.kind),
                                             request.currentUri));
    }

    OccurrenceCollector collector;

    if (request.target.kind == TargetKind::Local)
    {
        CollectLocalOccurrences(request.target, request.includeDeclaration, collector);
    }
    else if (request.target.kind == TargetKind::ClassMember)
    {
        CollectClassMemberOccurrences(request, collector);
    }
    else if (request.target.kind == TargetKind::NamespaceSymbol)
    {
        CollectNamespaceOccurrences(request, collector);
    }
    else
    {
        CollectGlobalOccurrences(request, collector);
    }

    if (request.logger && request.logger->IsTraceEnabled())
    {
        request.logger->LogTrace(fmt::format("[TargetResolution] Found {} occurrences for target '{}'",
                                             collector.results.size(), request.target.name));
    }

    return collector.results;
}

} // namespace angel_lsp::analysis
