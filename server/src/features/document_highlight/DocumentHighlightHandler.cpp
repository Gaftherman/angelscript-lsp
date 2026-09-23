#include "features/document_highlight/DocumentHighlightHandler.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include <algorithm>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
namespace
{


/**
 * @brief Checks whether a definition's declaring scope is inside any function/method/lambda body.
 * @param[in] defScope Definition declaring scope.
 * @return True if enclosed within a function scope.
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
 * @param[in] excludeName Optional class name to exclude.
 * @return Enclosing class name or empty string if not in a class.
 */
std::string GetEnclosingClassName(const analysis::SymbolTable& symbolTable, const std::string& uri, uint32_t line,
                                  const std::string& excludeName = "")
{
    std::string enclosingClass;
    symbolTable.ForEachSymbolInFile(
        uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if ((sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface) &&
                    sym.fileUri == uri && (excludeName.empty() || sym.name != excludeName))
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
 * @brief Extracts source code substring corresponding to an AST node.
 * @param[in] node AST node.
 * @param[in] sourceCode Source text.
 * @return Node text or empty string.
 */
std::string GetNodeText(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node) || sourceCode.empty())
    {
        return "";
    }
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start < sourceCode.size() && end <= sourceCode.size() && start < end)
    {
        return std::string(sourceCode.substr(start, end - start));
    }
    return "";
}

/**
 * @brief Checks if an AST node type represents an identifier or type reference.
 * @param[in] nodeType Type string of AST node.
 * @return True if node is an identifier or type candidate.
 */
bool IsIdentifierOrType(std::string_view nodeType) noexcept
{
    return nodeType == "identifier" || nodeType == "primitive_type" || nodeType == "scoped_identifier";
}

/**
 * @brief Searches for preceding identifier node when cursor is on trailing edge.
 * @param[in] rootNode Root of tree.
 * @param[in] position Cursor position.
 * @param[out] nodeType Detected node type.
 * @return Node if found, or null node.
 */
TSNode FindPrecedingIdentifierNode(TSNode rootNode, lsp::Position position, std::string_view& nodeType) noexcept
{
    if (position.character == 0)
    {
        return TSNode{};
    }
    TSPoint prevPoint = {position.line, position.character - 1};
    TSNode prevNode = ts_node_descendant_for_point_range(rootNode, prevPoint, prevPoint);
    if (!ts_node_is_null(prevNode))
    {
        std::string_view prevType = ts_node_type(prevNode);
        if (IsIdentifierOrType(prevType))
        {
            nodeType = prevType;
            return prevNode;
        }
    }
    return TSNode{};
}

/**
 * @brief Extracts leaf identifier from scoped identifier node.
 * @param[in] node Candidate node.
 * @param[in] point Cursor point.
 * @param[in,out] nodeType Type string of node.
 * @return Leaf identifier node or original node.
 */
TSNode ResolveIdentifierLeaf(TSNode node, TSPoint point, std::string_view& nodeType) noexcept
{
    if (nodeType == "scoped_identifier")
    {
        TSNode leaf = ts_node_descendant_for_point_range(node, point, point);
        if (!ts_node_is_null(leaf) && std::string_view(ts_node_type(leaf)) == "identifier")
        {
            nodeType = "identifier";
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
std::string GetNodeTextAt(const std::string& sourceCode, TSTree* tree, lsp::Position position, TSNode& outNode)
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

    std::string_view nodeType = ts_node_type(node);
    if (!IsIdentifierOrType(nodeType))
    {
        TSNode prev = FindPrecedingIdentifierNode(rootNode, position, nodeType);
        if (!ts_node_is_null(prev))
        {
            node = prev;
        }
    }

    node = ResolveIdentifierLeaf(node, point, nodeType);
    if (!IsIdentifierOrType(nodeType))
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
    const analysis::Scope* definingScope = nullptr;
    analysis::LocalDefinition localDef;
    std::string localUri;

    // Class member
    std::string declaringClass;
    std::vector<std::string> relatedClasses;

    // Namespace symbol
    std::string declaringNamespace;
};

/**
 * @brief Checks if an AST node is contained within another AST node range.
 * @param[in] inner Enclosed candidate node.
 * @param[in] outer Enclosing boundary node.
 * @return True if inner node is within outer node bounds.
 */
bool IsNodeContained(TSNode inner, TSNode outer) noexcept
{
    if (ts_node_is_null(inner) || ts_node_is_null(outer))
    {
        return false;
    }
    return ts_node_start_byte(inner) >= ts_node_start_byte(outer) && ts_node_end_byte(inner) <= ts_node_end_byte(outer);
}

/**
 * @brief Checks whether leaf node is the name of a declaration.
 * @param[in] leaf AST leaf node.
 * @return DocumentHighlightKind if leaf is a declaration identifier.
 */
/**
 * @brief Checks if node type is a variable or parameter declarator.
 * @param[in] pType Parent node type.
 * @return True if declarator node.
 */
bool IsVariableOrParamDeclarator(std::string_view pType) noexcept
{
    return pType == "variable_declarator" || pType == "parameter" || pType == "foreach_variable";
}

/**
 * @brief Checks if node type is a callable or type-level declaration.
 * @param[in] pType Parent node type.
 * @return True if callable or type declaration node.
 */
bool IsCallableOrTypeDeclaration(std::string_view pType) noexcept
{
    return pType == "func_declaration" || pType == "interface_method" || pType == "funcdef_declaration" ||
           pType == "class_declaration" || pType == "mixin_declaration" || pType == "interface_declaration" ||
           pType == "namespace_declaration" || pType == "enum_declaration" || pType == "typedef_declaration" ||
           pType == "enum_member";
}

/**
 * @brief Checks whether leaf node is the name of a declaration.
 * @param[in] leaf AST leaf node.
 * @return DocumentHighlightKind if leaf is a declaration identifier.
 */
std::optional<lsp::DocumentHighlightKind> ClassifyDeclarationNode(TSNode leaf)
{
    TSNode parent = ts_node_parent(leaf);
    if (ts_node_is_null(parent))
    {
        return std::nullopt;
    }
    std::string_view pType = ts_node_type(parent);
    if (IsVariableOrParamDeclarator(pType))
    {
        TSNode nameNode = parser::GetChildByField(parent, parser::fields::Name);
        if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
        {
            return lsp::DocumentHighlightKind::Write;
        }
    }
    if (IsCallableOrTypeDeclaration(pType))
    {
        TSNode nameNode = parser::GetChildByField(parent, parser::fields::Name);
        if (!ts_node_is_null(nameNode) && IsNodeContained(leaf, nameNode))
        {
            return lsp::DocumentHighlightKind::Text;
        }
    }
    return std::nullopt;
}

/**
 * @brief Checks whether leaf node is part of a type annotation.
 * @param[in] leaf AST leaf node.
 * @return True if node is in a type annotation.
 */
bool IsTypeAnnotationNode(TSNode leaf)
{
    for (TSNode cur = leaf; !ts_node_is_null(cur); cur = ts_node_parent(cur))
    {
        std::string_view cType = ts_node_type(cur);
        if (cType == "type" || cType == "datatype" || cType == "base_class_list" || cType == "template_type_list")
        {
            TSNode curParent = ts_node_parent(cur);
            if (!ts_node_is_null(curParent) && std::string_view(ts_node_type(curParent)) == "variable_declarator")
            {
                return false;
            }
            return true;
        }
        if (cType == "statement_block" || cType == "func_declaration" || cType == "class_body")
        {
            break;
        }
    }
    return false;
}

/**
 * @brief Checks if node is mutated by unary prefix or postfix ++ / --.
 * @param[in] cur Ancestor node.
 * @param[in] leaf Target node.
 * @param[in] sourceCode Source text.
 * @return Write highlight kind if mutated, nullopt otherwise.
 */
std::optional<lsp::DocumentHighlightKind> CheckUnaryMutation(TSNode cur, TSNode leaf, std::string_view sourceCode)
{
    std::string_view cType = ts_node_type(cur);
    if (cType == "postfix_expression")
    {
        TSNode operand = parser::GetChildByField(cur, parser::fields::Operand);
        if (!ts_node_is_null(operand) && IsNodeContained(leaf, operand))
        {
            return lsp::DocumentHighlightKind::Write;
        }
    }
    if (cType == "unary_expression")
    {
        TSNode opNode = parser::GetChildByField(cur, parser::fields::Operator);
        if (!ts_node_is_null(opNode))
        {
            uint32_t opStart = ts_node_start_byte(opNode);
            uint32_t opEnd = ts_node_end_byte(opNode);
            if (opStart < sourceCode.size() && opEnd <= sourceCode.size())
            {
                std::string_view opText = sourceCode.substr(opStart, opEnd - opStart);
                if (opText == "++" || opText == "--")
                {
                    TSNode operand = parser::GetChildByField(cur, parser::fields::Operand);
                    if (!ts_node_is_null(operand) && IsNodeContained(leaf, operand))
                    {
                        return lsp::DocumentHighlightKind::Write;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Checks if node is in an assignment expression.
 * @param[in] cur Ancestor node.
 * @param[in] leaf Target node.
 * @return Highlight kind if inside assignment expression, nullopt otherwise.
 */
std::optional<lsp::DocumentHighlightKind> CheckAssignmentMutation(TSNode cur, TSNode leaf)
{
    if (std::string_view(ts_node_type(cur)) != "assignment_expression")
    {
        return std::nullopt;
    }
    TSNode leftNode = parser::GetChildByField(cur, parser::fields::Left);
    if (!ts_node_is_null(leftNode) && IsNodeContained(leaf, leftNode))
    {
        if (std::string_view(ts_node_type(leftNode)) == "member_expression")
        {
            TSNode objNode = parser::GetChildByField(leftNode, parser::fields::Object);
            TSNode memNode = parser::GetChildByField(leftNode, parser::fields::Member);
            if (!ts_node_is_null(objNode) && IsNodeContained(leaf, objNode))
            {
                return lsp::DocumentHighlightKind::Read;
            }
            if (!ts_node_is_null(memNode) && IsNodeContained(leaf, memNode))
            {
                return lsp::DocumentHighlightKind::Write;
            }
        }
        if (std::string_view(ts_node_type(leftNode)) == "index_expression")
        {
            return lsp::DocumentHighlightKind::Read;
        }
        return lsp::DocumentHighlightKind::Write;
    }
    TSNode rightNode = parser::GetChildByField(cur, parser::fields::Right);
    if (!ts_node_is_null(rightNode) && IsNodeContained(leaf, rightNode))
    {
        return lsp::DocumentHighlightKind::Read;
    }
    return std::nullopt;
}

/**
 * @brief Finds the 0-based argument index of leaf within an argument_list.
 * @param[in] argList argument_list AST node.
 * @param[in] leaf Target identifier leaf node.
 * @return Argument index or nullopt if not an argument.
 */
std::optional<uint32_t> FindArgumentIndex(TSNode argList, TSNode leaf) noexcept
{
    uint32_t childCount = ts_node_child_count(argList);
    uint32_t argIndex = 0;
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode argChild = ts_node_child(argList, i);
        std::string_view childType = ts_node_type(argChild);
        if (childType == "(" || childType == ")" || childType == ",")
        {
            continue;
        }
        if (IsNodeContained(leaf, argChild))
        {
            return argIndex;
        }
        argIndex++;
    }
    return std::nullopt;
}

struct CalleeInfo
{
    std::string calleeName;
    std::string receiverType;
};

/**
 * @brief Resolves receiver type of an object expression text.
 * @param[in] oText Object text.
 * @param[in] request Highlight request.
 * @param[in] line 0-based source line.
 * @return Receiver type name.
 */
std::string ResolveCallReceiverType(const std::string& oText, const DocumentHighlightRequest& request, uint32_t line)
{
    if (oText == "this")
    {
        return GetEnclosingClassName(request.symbolTable, request.uri, line);
    }
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    if (rootScope)
    {
        const analysis::Scope* s = FindInnermostScope(rootScope.get(), line, 0);
        if (s)
        {
            const analysis::LocalDefinition* oDef = analysis::ResolveInScope(s, oText);
            if (oDef && !oDef->typeName.empty())
            {
                return analysis::CleanBaseType(oDef->typeName);
            }
        }
    }
    auto gSyms = request.symbolTable.FindSymbols(oText);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(gs.GetVariable().typeName);
        }
    }
    return "";
}

/**
 * @brief Resolves callee function name and optional receiver type for a call node.
 * @param[in] funcNode Function AST node in call_expression.
 * @param[in] request Highlight request.
 * @param[in] line 0-based line number.
 * @param[out] info Callee and receiver descriptor.
 */
void ResolveCalleeInfo(TSNode funcNode, const DocumentHighlightRequest& request, uint32_t line, CalleeInfo& info)
{
    if (std::string_view(ts_node_type(funcNode)) == "member_expression")
    {
        TSNode memNode = parser::GetChildByField(funcNode, parser::fields::Member);
        TSNode objNode = parser::GetChildByField(funcNode, parser::fields::Object);
        if (!ts_node_is_null(memNode))
        {
            info.calleeName = GetNodeText(memNode, request.sourceCode);
        }
        if (!ts_node_is_null(objNode))
        {
            std::string oText = GetNodeText(objNode, request.sourceCode);
            info.receiverType = ResolveCallReceiverType(oText, request, line);
        }
    }
    else
    {
        info.calleeName = GetNodeText(funcNode, request.sourceCode);
    }
}

/**
 * @brief Checks if argument at argIndex corresponds to an out or inout parameter.
 * @param[in] info Callee descriptor.
 * @param[in] argIndex 0-based argument index.
 * @param[in] symbolTable Global symbol table.
 * @return True if parameter has out or non-const reference semantics.
 */
bool IsOutOrInOutParameter(const CalleeInfo& info, uint32_t argIndex, const analysis::SymbolTable& symbolTable)
{
    std::vector<analysis::Symbol> matches;
    if (!info.receiverType.empty())
    {
        auto related = analysis::GetAllRelatedClasses(info.receiverType, symbolTable);
        for (const auto& cls : related)
        {
            auto found = symbolTable.FindSymbols(cls + "::" + info.calleeName);
            matches.insert(matches.end(), found.begin(), found.end());
        }
    }
    else if (!info.calleeName.empty())
    {
        matches = symbolTable.FindSymbols(info.calleeName);
    }

    for (const auto& sym : matches)
    {
        if (sym.type == analysis::SymbolType::Function)
        {
            const auto& sig = sym.GetFunction();
            if (argIndex < sig.parameters.size())
            {
                const auto& param = sig.parameters[argIndex];
                if (param.modifier == analysis::ParameterModifier::Out ||
                    param.modifier == analysis::ParameterModifier::InOut)
                {
                    return true;
                }
                if (param.isReference && !param.isConst && param.modifier != analysis::ParameterModifier::In)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief Checks if node is passed as an out/inout argument in a call expression.
 * @param[in] cur Current ancestor node.
 * @param[in] leaf Target node.
 * @param[in] request Highlight request.
 * @param[in] line 0-based source line.
 * @return Write highlight kind if out/inout argument, nullopt otherwise.
 */
std::optional<lsp::DocumentHighlightKind>
CheckCallArgumentMutation(TSNode cur, TSNode leaf, const DocumentHighlightRequest& request, uint32_t line)
{
    if (std::string_view(ts_node_type(cur)) != "argument_list")
    {
        return std::nullopt;
    }
    TSNode callNode = ts_node_parent(cur);
    if (ts_node_is_null(callNode) || std::string_view(ts_node_type(callNode)) != "call_expression")
    {
        return std::nullopt;
    }
    auto argIndex = FindArgumentIndex(cur, leaf);
    if (!argIndex.has_value())
    {
        return std::nullopt;
    }
    TSNode funcNode = parser::GetChildByField(callNode, parser::fields::Function);
    if (ts_node_is_null(funcNode))
    {
        return std::nullopt;
    }
    CalleeInfo info;
    ResolveCalleeInfo(funcNode, request, line, info);
    if (IsOutOrInOutParameter(info, *argIndex, request.symbolTable))
    {
        return lsp::DocumentHighlightKind::Write;
    }
    return std::nullopt;
}

/**
 * @brief Walks ancestor expressions to check for mutating operations or function arguments.
 * @param[in] leaf Identifier leaf node.
 * @param[in] request Highlight request.
 * @param[in] line Source line.
 * @return Classified highlight kind.
 */
lsp::DocumentHighlightKind ClassifyAncestorExpressions(TSNode leaf, const DocumentHighlightRequest& request,
                                                       uint32_t line)
{
    for (TSNode cur = leaf; !ts_node_is_null(cur); cur = ts_node_parent(cur))
    {
        if (auto res = CheckUnaryMutation(cur, leaf, request.sourceCode))
        {
            return *res;
        }
        if (auto res = CheckAssignmentMutation(cur, leaf))
        {
            return *res;
        }
        if (auto res = CheckCallArgumentMutation(cur, leaf, request, line))
        {
            return *res;
        }
        if (std::string_view(ts_node_type(cur)) == "statement_block")
        {
            break;
        }
    }
    return lsp::DocumentHighlightKind::Read;
}

/**
 * @brief Classifies the AST context of an occurrence into Write, Read, or Text highlight kinds.
 * @param[in] request Highlight request.
 * @param[in] range Occurrence source range.
 * @return Document highlight kind.
 */
lsp::DocumentHighlightKind ClassifyOccurrence(const DocumentHighlightRequest& request, const lsp::Range& range)
{
    if (!request.tree || request.sourceCode.empty())
    {
        return lsp::DocumentHighlightKind::Read;
    }

    TSNode rootNode = ts_tree_root_node(request.tree);
    TSPoint pt = {range.start.line, range.start.character};
    TSNode leaf = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(leaf))
    {
        return lsp::DocumentHighlightKind::Read;
    }

    if (std::string_view(ts_node_type(leaf)) == "scoped_identifier")
    {
        TSNode inner = ts_node_descendant_for_point_range(leaf, pt, pt);
        if (!ts_node_is_null(inner))
        {
            leaf = inner;
        }
    }

    if (auto declKind = ClassifyDeclarationNode(leaf))
    {
        return *declKind;
    }
    if (IsTypeAnnotationNode(leaf))
    {
        return lsp::DocumentHighlightKind::Text;
    }
    return ClassifyAncestorExpressions(leaf, request, range.start.line);
}

/**
 * @brief Resolves receiver type name from explicit object expression node.
 * @param[in] objectNode Object AST node.
 * @param[in] request Highlight request.
 * @param[in] rootScope Document root scope.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveExplicitReceiverType(TSNode objectNode, const DocumentHighlightRequest& request,
                                        const std::shared_ptr<const analysis::Scope>& rootScope)
{
    uint32_t objStart = ts_node_start_byte(objectNode);
    uint32_t objEnd = ts_node_end_byte(objectNode);
    if (objStart >= request.sourceCode.size() || objEnd > request.sourceCode.size())
    {
        return "";
    }
    std::string objText = request.sourceCode.substr(objStart, objEnd - objStart);
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
 * @param[in] node Cursor identifier node.
 * @param[in] request Highlight request.
 * @param[in] rootScope Document root scope.
 * @param[out] target Target descriptor to populate.
 * @return True if cursor is on an explicit member access expression.
 */
bool ResolveExplicitMemberAccess(TSNode node, const DocumentHighlightRequest& request,
                                 const std::shared_ptr<const analysis::Scope>& rootScope, TargetDescriptor& target)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent) || std::string_view(ts_node_type(parent)) != "member_expression")
    {
        return false;
    }
    TSNode memNode = parser::GetChildByField(parent, parser::fields::Member);
    if (ts_node_is_null(memNode) ||
        (!ts_node_eq(memNode, node) && ts_node_start_byte(memNode) != ts_node_start_byte(node)))
    {
        return false;
    }
    TSNode objectNode = parser::GetChildByField(parent, parser::fields::Object);
    if (ts_node_is_null(objectNode))
    {
        return false;
    }
    std::string receiverTypeName = ResolveExplicitReceiverType(objectNode, request, rootScope);
    if (!receiverTypeName.empty())
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = receiverTypeName;
        target.relatedClasses = analysis::GetAllRelatedClasses(receiverTypeName, request.symbolTable);
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
    for (const analysis::Scope* cur = innerScope; cur != nullptr; cur = cur->parent)
    {
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
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Highlight request.
 * @param[in] rootScope Document root scope.
 * @param[out] target Target descriptor to populate.
 */
void ResolveLocalTarget(const std::string& nodeText, const DocumentHighlightRequest& request,
                        const std::shared_ptr<const analysis::Scope>& rootScope, TargetDescriptor& target)
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
        matchedDef = analysis::ResolveInScope(innerScope, nodeText, nullptr, false);
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
            return;
        }
    }
    std::string enclosingClass =
        GetEnclosingClassName(request.symbolTable, request.uri, request.position.line, nodeText);
    if (!enclosingClass.empty())
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = enclosingClass;
        target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
    }
}

/**
 * @brief Resolves target through enclosing class, interface, or namespace containers.
 * @param[in] node AST identifier node.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Highlight request.
 * @param[out] target Target descriptor to populate.
 */
void ResolveContainerTarget(TSNode node, const std::string& nodeText, const DocumentHighlightRequest& request,
                            TargetDescriptor& target)
{
    std::string enclosingClass =
        GetEnclosingClassName(request.symbolTable, request.uri, request.position.line, nodeText);
    if (!enclosingClass.empty())
    {
        target.kind = TargetKind::ClassMember;
        target.declaringClass = enclosingClass;
        target.relatedClasses = analysis::GetAllRelatedClasses(enclosingClass, request.symbolTable);
        return;
    }
    auto containers = analysis::GetEnclosingContainers(node, request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == analysis::ContainerKind::Class || container.kind == analysis::ContainerKind::Interface)
        {
            auto hierarchy = analysis::GetAllRelatedClasses(container.qualifiedName, request.symbolTable);
            for (const auto& cls : hierarchy)
            {
                if (request.symbolTable.HasSymbol(cls + "::" + nodeText))
                {
                    target.kind = TargetKind::ClassMember;
                    target.declaringClass = cls;
                    target.relatedClasses = std::move(hierarchy);
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
 * @brief Resolves target through symbolTable symbol definitions in the current document.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Highlight request.
 * @param[out] target Target descriptor to populate.
 */
void ResolveSymbolTableTarget(const std::string& nodeText, const DocumentHighlightRequest& request,
                              TargetDescriptor& target)
{
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
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
                            target.relatedClasses =
                                analysis::GetAllRelatedClasses(sym.containerName, request.symbolTable);
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
 * @brief Resolves the semantic target descriptor for the symbol under cursor.
 * @param[in] node AST identifier node.
 * @param[in] nodeText Symbol identifier text.
 * @param[in] request Highlight request.
 * @param[in] rootScope Document root scope.
 * @return Resolved TargetDescriptor.
 */
TargetDescriptor ResolveTarget(TSNode node, const std::string& nodeText, const DocumentHighlightRequest& request,
                               const std::shared_ptr<const analysis::Scope>& rootScope)
{
    TargetDescriptor target;
    target.name = nodeText;

    bool isExplicitMemberAccess = ResolveExplicitMemberAccess(node, request, rootScope, target);
    if (!isExplicitMemberAccess)
    {
        ResolveLocalTarget(nodeText, request, rootScope, target);
    }
    if (target.kind == TargetKind::GlobalSymbol && !isExplicitMemberAccess)
    {
        ResolveContainerTarget(node, nodeText, request, target);
    }
    if (target.kind == TargetKind::GlobalSymbol)
    {
        ResolveSymbolTableTarget(nodeText, request, target);
    }
    return target;
}

/**
 * @brief Accumulator for highlight coordinate ranges.
 */
struct HighlightRangeCollector
{
    std::vector<lsp::Range> ranges;
    std::set<std::pair<uint32_t, uint32_t>> seen;

    /**
     * @brief Adds a range if not already collected.
     * @param[in] sL Start line.
     * @param[in] sC Start character.
     * @param[in] eL End line.
     * @param[in] eC End character.
     */
    void Add(uint32_t sL, uint32_t sC, uint32_t eL, uint32_t eC)
    {
        if (seen.insert({sL, sC}).second)
        {
            ranges.push_back(lsp::Range{lsp::Position{sL, sC}, lsp::Position{eL, eC}});
        }
    }
};

/**
 * @brief Collects highlight ranges for a local variable or parameter.
 * @param[in] target Target descriptor.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectLocalHighlightRanges(const TargetDescriptor& target, HighlightRangeCollector& collector)
{
    collector.Add(target.localDef.startLine, target.localDef.startCharacter, target.localDef.endLine,
                  target.localDef.endCharacter);

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
                collector.Add(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter);
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
 * @brief Collects declarations of class members in the current file.
 * @param[in] relatedSet Set of related class names.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectClassMemberDeclarations(const std::unordered_set<std::string>& relatedSet, const TargetDescriptor& target,
                                    const DocumentHighlightRequest& request, HighlightRangeCollector& collector)
{
    for (const auto& clsName : relatedSet)
    {
        std::string qualifiedName = clsName + "::" + target.name;
        auto syms = request.symbolTable.FindSymbols(qualifiedName);
        for (const auto& sym : syms)
        {
            if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
            {
                bool hasSel = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0);
                uint32_t sL = hasSel ? sym.selectionRange.startLine : sym.startLine;
                uint32_t sC = hasSel ? sym.selectionRange.startCharacter : sym.startCharacter;
                uint32_t eL = hasSel ? sym.selectionRange.endLine : sym.endLine;
                uint32_t eC = hasSel ? sym.selectionRange.endCharacter : sym.endCharacter;
                collector.Add(sL, sC, eL, eC);
            }
        }
    }
}

/**
 * @brief Splits a dot-delimited expression string into member component parts.
 * @param[in] oText Dotted expression text.
 * @return Vector of component strings.
 */
std::vector<std::string> SplitDottedParts(const std::string& oText)
{
    std::vector<std::string> parts;
    size_t startPos = 0;
    while (startPos < oText.size())
    {
        size_t dotPos = oText.find('.', startPos);
        if (dotPos == std::string::npos)
        {
            parts.push_back(oText.substr(startPos));
            break;
        }
        parts.push_back(oText.substr(startPos, dotPos - startPos));
        startPos = dotPos + 1;
    }
    return parts;
}

/**
 * @brief Resolves type of root component in a dotted member chain.
 * @param[in] part Root identifier text.
 * @param[in] scope Current lexical scope.
 * @param[in] activeScope Innermost scope at cursor.
 * @param[in] request Highlight request.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveChainRootType(const std::string& part, const analysis::Scope* scope,
                                 const analysis::Scope* activeScope, const DocumentHighlightRequest& request)
{
    const analysis::LocalDefinition* pDef = analysis::ResolveInScope(scope, part);
    if (!pDef && activeScope)
    {
        pDef = analysis::ResolveInScope(activeScope, part);
    }
    if (pDef && !pDef->typeName.empty())
    {
        return analysis::CleanBaseType(pDef->typeName);
    }
    auto pSyms = request.symbolTable.FindSymbols(part);
    for (const auto& ps : pSyms)
    {
        if (ps.type == analysis::SymbolType::Variable && !ps.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(ps.GetVariable().typeName);
        }
    }
    return "";
}

/**
 * @brief Resolves type of subsequent member component in a dotted member chain.
 * @param[in] curType Type name of container/receiver.
 * @param[in] part Member identifier text.
 * @param[in] request Highlight request.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveChainMemberType(const std::string& curType, const std::string& part,
                                   const DocumentHighlightRequest& request)
{
    std::string qMem = curType + "::" + part;
    auto mSyms = request.symbolTable.FindSymbols(qMem);
    if (mSyms.empty())
    {
        mSyms = request.symbolTable.FindSymbols(part);
    }
    for (const auto& ms : mSyms)
    {
        if ((ms.type == analysis::SymbolType::Variable || ms.type == analysis::SymbolType::Property) &&
            !ms.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(ms.GetVariable().typeName);
        }
        if (ms.type == analysis::SymbolType::Function && !ms.GetFunction().returnType.empty())
        {
            return analysis::CleanBaseType(ms.GetFunction().returnType);
        }
    }
    return "";
}

/**
 * @brief Resolves type name of dotted member chain expressions.
 * @param[in] oText Dotted chain string.
 * @param[in] scope Current lexical scope.
 * @param[in] activeScope Active scope at position.
 * @param[in] request Highlight request.
 * @return Resolved type name or empty string.
 */
std::string ResolveDottedChainType(const std::string& oText, const analysis::Scope* scope,
                                   const analysis::Scope* activeScope, const DocumentHighlightRequest& request)
{
    auto parts = SplitDottedParts(oText);
    std::string curType;
    for (size_t p = 0; p < parts.size(); ++p)
    {
        if (p == 0)
        {
            curType = ResolveChainRootType(parts[p], scope, activeScope, request);
        }
        else if (!curType.empty())
        {
            curType = ResolveChainMemberType(curType, parts[p], request);
        }
    }
    return curType;
}

/**
 * @brief Resolves type name for a simple identifier expression in scope.
 * @param[in] oText Identifier text.
 * @param[in] scope Current lexical scope.
 * @param[in] activeScope Active scope at position.
 * @param[in] request Highlight request.
 * @return Cleaned base type name or empty string.
 */
std::string ResolveIdentifierReceiverType(const std::string& oText, const analysis::Scope* scope,
                                          const analysis::Scope* activeScope, const DocumentHighlightRequest& request)
{
    const analysis::LocalDefinition* oDef = analysis::ResolveInScope(scope, oText);
    if (!oDef && activeScope)
    {
        oDef = analysis::ResolveInScope(activeScope, oText);
    }
    if (oDef && !oDef->typeName.empty())
    {
        return analysis::CleanBaseType(oDef->typeName);
    }
    auto gSyms = request.symbolTable.FindSymbols(oText);
    for (const auto& gs : gSyms)
    {
        if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
        {
            return analysis::CleanBaseType(gs.GetVariable().typeName);
        }
    }
    return "";
}

/**
 * @brief Finds enclosing member_expression parent, stopping at boundary containers.
 * @param[in] refNode Leaf node.
 * @return Parent member_expression node or null node.
 */
TSNode FindMemberExpressionParent(TSNode refNode) noexcept
{
    TSNode exprParent = ts_node_parent(refNode);
    while (!ts_node_is_null(exprParent) && std::string_view(ts_node_type(exprParent)) != "member_expression")
    {
        std::string_view pType = ts_node_type(exprParent);
        if (pType == "class_declaration" || pType == "func_declaration" || pType == "lambda_expression")
        {
            return TSNode{};
        }
        exprParent = ts_node_parent(exprParent);
    }
    return exprParent;
}

/**
 * @brief Resolves identifier leaf AST node corresponding to reference.
 * @param[in] rootNode AST root node.
 * @param[in] ref Reference descriptor.
 * @param[in] sourceCode Source text.
 * @return Matching identifier node or null node.
 */
TSNode ResolveRefNodeLeaf(TSNode rootNode, const analysis::LocalReference& ref, std::string_view sourceCode) noexcept
{
    TSPoint pt = {ref.startLine, ref.startCharacter};
    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(refNode))
    {
        return TSNode{};
    }
    if (std::string_view(ts_node_type(refNode)) != "identifier")
    {
        uint32_t cCount = ts_node_child_count(refNode);
        for (uint32_t c = 0; c < cCount; ++c)
        {
            TSNode ch = ts_node_child(refNode, c);
            if (std::string_view(ts_node_type(ch)) == "identifier" && GetNodeText(ch, sourceCode) == ref.name)
            {
                return ch;
            }
        }
    }
    return refNode;
}

/**
 * @brief Resolves receiver type name for member reference.
 * @param[in] ref Reference descriptor.
 * @param[in] scope Current scope.
 * @param[in] activeScope Innermost scope at position.
 * @param[in] request Highlight request.
 * @return Receiver type name or empty string.
 */
std::string ResolveMemberRefReceiverType(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                         const analysis::Scope* activeScope, const DocumentHighlightRequest& request)
{
    if (!request.tree)
    {
        return "";
    }
    TSNode rootNode = ts_tree_root_node(request.tree);
    TSNode refNode = ResolveRefNodeLeaf(rootNode, ref, request.sourceCode);
    if (ts_node_is_null(refNode))
    {
        return "";
    }
    TSNode exprParent = FindMemberExpressionParent(refNode);
    if (ts_node_is_null(exprParent))
    {
        return "";
    }
    TSNode objNode = parser::GetChildByField(exprParent, parser::fields::Object);
    if (ts_node_is_null(objNode) && ts_node_named_child_count(exprParent) > 0)
    {
        objNode = ts_node_named_child(exprParent, 0);
    }
    if (ts_node_is_null(objNode))
    {
        return "";
    }
    std::string rType =
        analysis::ResolveExpressionType(objNode, {scope, request.symbolTable, request.sourceCode, request.uri});
    if (rType.empty() && activeScope && activeScope != scope)
    {
        rType = analysis::ResolveExpressionType(objNode,
                                                {activeScope, request.symbolTable, request.sourceCode, request.uri});
    }
    if (!rType.empty())
    {
        return analysis::CleanBaseType(rType);
    }
    uint32_t oStart = ts_node_start_byte(objNode);
    uint32_t oEnd = ts_node_end_byte(objNode);
    if (oStart >= request.sourceCode.size() || oEnd > request.sourceCode.size())
    {
        return "";
    }
    std::string oText = request.sourceCode.substr(oStart, oEnd - oStart);
    if (oText == "this")
    {
        return GetEnclosingClassName(request.symbolTable, request.uri, ref.startLine, ref.name);
    }
    if (oText.find('.') != std::string::npos)
    {
        return analysis::CleanBaseType(ResolveDottedChainType(oText, scope, activeScope, request));
    }
    return analysis::CleanBaseType(ResolveIdentifierReceiverType(oText, scope, activeScope, request));
}

/**
 * @brief Checks if a reference is syntactically a member access expression.
 * @param[in] ref Reference descriptor.
 * @param[in] tree AST tree.
 * @return True if reference is a member access.
 */
bool IsMemberAccessNode(const analysis::LocalReference& ref, TSTree* tree) noexcept
{
    if (ref.isMemberAccess)
    {
        return true;
    }
    if (!tree)
    {
        return false;
    }
    TSNode rootNode = ts_tree_root_node(tree);
    TSPoint pt = {ref.startLine, ref.startCharacter};
    TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
    if (ts_node_is_null(refNode))
    {
        return false;
    }
    TSNode p = ts_node_parent(refNode);
    return !ts_node_is_null(p) && std::string_view(ts_node_type(p)) == "member_expression";
}

/**
 * @brief Context for class member reference matching.
 */
struct MemberHighlightContext
{
    const TargetDescriptor& target;
    const std::unordered_set<std::string>& relatedSet;
    const DocumentHighlightRequest& request;
    const analysis::Scope* rootScope = nullptr;
};

/**
 * @brief Evaluates whether a reference matches target class member.
 * @param[in] ref Reference under test.
 * @param[in] scope Current scope.
 * @param[in] ctx Member highlight context.
 * @return True if reference is a match.
 */
bool IsClassMemberReferenceMatch(const analysis::LocalReference& ref, const analysis::Scope* scope,
                                 const MemberHighlightContext& ctx)
{
    const analysis::Scope* activeScope = FindInnermostScope(ctx.rootScope, ref.startLine, ref.startCharacter);
    if (!activeScope)
    {
        activeScope = scope;
    }
    if (IsMemberAccessNode(ref, ctx.request.tree))
    {
        std::string rType = ResolveMemberRefReceiverType(ref, scope, activeScope, ctx.request);
        return !rType.empty() && ctx.relatedSet.contains(rType);
    }
    std::string encClass = GetEnclosingClassName(ctx.request.symbolTable, ctx.request.uri, ref.startLine, ref.name);
    if (ctx.relatedSet.contains(encClass))
    {
        const analysis::LocalDefinition* localShadow = analysis::ResolveInScope(activeScope, ctx.target.name);
        return !localShadow || localShadow->kind == analysis::LocalDefinitionKind::Field ||
               localShadow->kind == analysis::LocalDefinitionKind::Method;
    }
    return false;
}

/**
 * @brief Collects all highlight ranges for a class member in the current document.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in] rootScope Root scope.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectClassMemberHighlightRanges(const TargetDescriptor& target, const DocumentHighlightRequest& request,
                                       const analysis::Scope* rootScope, HighlightRangeCollector& collector)
{
    std::unordered_set<std::string> relatedSet(target.relatedClasses.begin(), target.relatedClasses.end());
    if (!target.declaringClass.empty())
    {
        relatedSet.insert(target.declaringClass);
    }
    CollectClassMemberDeclarations(relatedSet, target, request, collector);
    if (!rootScope)
    {
        return;
    }
    MemberHighlightContext ctx{
        .target = target,
        .relatedSet = relatedSet,
        .request = request,
        .rootScope = rootScope,
    };
    std::vector<const analysis::Scope*> stack{rootScope};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name == target.name && IsClassMemberReferenceMatch(ref, s, ctx))
            {
                collector.Add(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter);
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
 * @brief Checks if reference is enclosed within namespace scope or is a qualified scoped_identifier.
 * @param[in] ref Reference descriptor.
 * @param[in] nsRanges Declaring namespace line ranges.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @return True if reference is inside namespace.
 */
bool IsRefInsideNamespaceScope(const analysis::LocalReference& ref,
                               const std::vector<std::pair<uint32_t, uint32_t>>& nsRanges,
                               const TargetDescriptor& target, const DocumentHighlightRequest& request)
{
    for (const auto& nr : nsRanges)
    {
        if (ref.startLine >= nr.first && ref.startLine <= nr.second)
        {
            return true;
        }
    }
    if (request.tree)
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
                    if (scoped == target.qualifiedName)
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
 * @brief Checks if a local definition shadows a namespace or global symbol.
 * @param[in] scope Current lexical scope.
 * @param[in] name Symbol name.
 * @param[in] rootScope Root scope.
 * @return True if shadowed by a function-local variable or parameter.
 */
bool IsShadowedByFunctionLocal(const analysis::Scope* scope, const std::string& name, const analysis::Scope* rootScope)
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
    const analysis::Scope* defScope = analysis::FindScopeDeclaringDefinition(rootScope, *localDef);
    return defScope && IsDeclaredInFunctionScope(defScope);
}

/**
 * @brief Collects namespace symbol declarations in current file.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectNamespaceDeclarations(const TargetDescriptor& target, const DocumentHighlightRequest& request,
                                  HighlightRangeCollector& collector)
{
    auto syms = request.symbolTable.FindSymbols(target.qualifiedName);
    for (const auto& sym : syms)
    {
        if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
        {
            bool hasSel = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0);
            uint32_t sL = hasSel ? sym.selectionRange.startLine : sym.startLine;
            uint32_t sC = hasSel ? sym.selectionRange.startCharacter : sym.startCharacter;
            uint32_t eL = hasSel ? sym.selectionRange.endLine : sym.endLine;
            uint32_t eC = hasSel ? sym.selectionRange.endCharacter : sym.endCharacter;
            collector.Add(sL, sC, eL, eC);
        }
    }
}

/**
 * @brief Collects line ranges of declaring namespace across current file.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @return Vector of line range pairs.
 */
std::vector<std::pair<uint32_t, uint32_t>> GetNamespaceSpans(const TargetDescriptor& target,
                                                             const DocumentHighlightRequest& request)
{
    std::vector<std::pair<uint32_t, uint32_t>> nsRanges;
    request.symbolTable.ForEachSymbolInFile(
        request.uri,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& sList)
        {
            for (const auto& s : sList)
            {
                if (s.type == analysis::SymbolType::Namespace && s.fileUri == request.uri &&
                    (s.name == target.declaringNamespace || s.qualifiedName == target.declaringNamespace))
                {
                    nsRanges.push_back({s.startLine, s.endLine});
                }
            }
        });
    return nsRanges;
}

/**
 * @brief Collects all highlight ranges for a namespace symbol in the current document.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in] rootScope Root scope.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectNamespaceHighlightRanges(const TargetDescriptor& target, const DocumentHighlightRequest& request,
                                     const analysis::Scope* rootScope, HighlightRangeCollector& collector)
{
    CollectNamespaceDeclarations(target, request, collector);
    if (!rootScope)
    {
        return;
    }
    auto nsRanges = GetNamespaceSpans(target, request);
    std::vector<const analysis::Scope*> stack{rootScope};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name == target.name && !ref.isMemberAccess &&
                IsRefInsideNamespaceScope(ref, nsRanges, target, request) &&
                !IsShadowedByFunctionLocal(s, target.name, rootScope))
            {
                collector.Add(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter);
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
 * @brief Collects global symbol declarations in current file.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectGlobalDeclarations(const TargetDescriptor& target, const DocumentHighlightRequest& request,
                               HighlightRangeCollector& collector)
{
    auto syms = request.symbolTable.FindSymbols(target.name);
    for (const auto& sym : syms)
    {
        if (sym.fileUri == request.uri && sym.type != analysis::SymbolType::CallReference)
        {
            bool hasSel = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0);
            uint32_t sL = hasSel ? sym.selectionRange.startLine : sym.startLine;
            uint32_t sC = hasSel ? sym.selectionRange.startCharacter : sym.startCharacter;
            uint32_t eL = hasSel ? sym.selectionRange.endLine : sym.endLine;
            uint32_t eC = hasSel ? sym.selectionRange.endCharacter : sym.endCharacter;
            collector.Add(sL, sC, eL, eC);
        }
    }
}

/**
 * @brief Collects all highlight ranges for a global symbol in the current document.
 * @param[in] target Target descriptor.
 * @param[in] request Highlight request.
 * @param[in] rootScope Root scope.
 * @param[in,out] collector Coordinate accumulator.
 */
void CollectGlobalHighlightRanges(const TargetDescriptor& target, const DocumentHighlightRequest& request,
                                  const analysis::Scope* rootScope, HighlightRangeCollector& collector)
{
    CollectGlobalDeclarations(target, request, collector);
    if (!rootScope)
    {
        return;
    }
    std::vector<const analysis::Scope*> stack{rootScope};
    while (!stack.empty())
    {
        const analysis::Scope* s = stack.back();
        stack.pop_back();

        for (const auto& ref : s->references)
        {
            if (ref.name == target.name && !ref.isMemberAccess && !IsShadowedByFunctionLocal(s, target.name, rootScope))
            {
                collector.Add(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter);
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

} // namespace

std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request)
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
    TargetDescriptor target = ResolveTarget(node, nodeText, request, rootScope);

    HighlightRangeCollector collector;
    if (target.kind == TargetKind::Local)
    {
        CollectLocalHighlightRanges(target, collector);
    }
    else if (target.kind == TargetKind::ClassMember)
    {
        CollectClassMemberHighlightRanges(target, request, rootScope.get(), collector);
    }
    else if (target.kind == TargetKind::NamespaceSymbol)
    {
        CollectNamespaceHighlightRanges(target, request, rootScope.get(), collector);
    }
    else
    {
        CollectGlobalHighlightRanges(target, request, rootScope.get(), collector);
    }

    if (collector.ranges.empty())
    {
        return std::nullopt;
    }

    DocumentHighlightResult results;
    results.reserve(collector.ranges.size());
    for (const auto& r : collector.ranges)
    {
        results.push_back(lsp::DocumentHighlight{
            .range = r,
            .kind = ClassifyOccurrence(request, r),
        });
    }

    std::sort(results.begin(), results.end(),
              [](const lsp::DocumentHighlight& a, const lsp::DocumentHighlight& b)
              {
                  if (a.range.start.line != b.range.start.line)
                  {
                      return a.range.start.line < b.range.start.line;
                  }
                  return a.range.start.character < b.range.start.character;
              });

    return results;
}

} // namespace angel_lsp::features
