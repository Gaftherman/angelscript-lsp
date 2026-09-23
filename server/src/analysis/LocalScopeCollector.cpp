#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"

#include "parser/GrammarNames.h"
#include <algorithm>
#include <cstring>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
LocalScopeCollector::LocalScopeCollector(angel_lsp::utils::LspLogger* logger) : m_logger(logger), m_localsQuery(nullptr)
{
    const TSLanguage* lang = tree_sitter_angelscript();

    m_symMemberExpression = ts_language_symbol_for_name(lang, "member_expression",
                                                        static_cast<uint32_t>(strlen("member_expression")), true);
    m_symFuncDeclaration =
        ts_language_symbol_for_name(lang, "func_declaration", static_cast<uint32_t>(strlen("func_declaration")), true);
    m_symLambdaExpression = ts_language_symbol_for_name(lang, "lambda_expression",
                                                        static_cast<uint32_t>(strlen("lambda_expression")), true);
    m_symClassBody = ts_language_symbol_for_name(lang, "class_body", static_cast<uint32_t>(strlen("class_body")), true);
    m_symInterfaceBody =
        ts_language_symbol_for_name(lang, "interface_body", static_cast<uint32_t>(strlen("interface_body")), true);
    m_symNamespaceBody =
        ts_language_symbol_for_name(lang, "namespace_body", static_cast<uint32_t>(strlen("namespace_body")), true);
    m_symScript = ts_language_symbol_for_name(lang, "script", static_cast<uint32_t>(strlen("script")), true);
    m_symVariableDeclarator = ts_language_symbol_for_name(lang, "variable_declarator",
                                                          static_cast<uint32_t>(strlen("variable_declarator")), true);
    m_symParameter = ts_language_symbol_for_name(lang, "parameter", static_cast<uint32_t>(strlen("parameter")), true);
    m_symForeachVariable =
        ts_language_symbol_for_name(lang, "foreach_variable", static_cast<uint32_t>(strlen("foreach_variable")), true);

    m_localsQuery = parser::QueryRegistry::GetLocalsQuery();
    if (!m_localsQuery)
    {
        if (m_logger)
            m_logger->LogError("Failed to get LOCALS_QUERY from QueryRegistry");
        return;
    }

    InitializeCaptureKinds(m_captureKinds, m_definitionKinds);
}

void LocalScopeCollector::InitializeCaptureKinds(std::vector<CaptureKind>& kinds,
                                                 std::vector<LocalDefinitionKind>& defKinds)
{
    uint32_t captureCount = ts_query_capture_count(m_localsQuery);
    kinds.assign(captureCount, CaptureKind::None);
    defKinds.assign(captureCount, LocalDefinitionKind::Variable);

    for (uint32_t i = 0; i < captureCount; ++i)
    {
        uint32_t nameLen = 0;
        const char* name = ts_query_capture_name_for_id(m_localsQuery, i, &nameLen);
        std::string_view captureName(name, nameLen);

        if (captureName == "local.scope")
        {
            kinds[i] = CaptureKind::Scope;
        }
        else if (captureName == "local.reference")
        {
            kinds[i] = CaptureKind::Reference;
        }
        else if (captureName.starts_with("local.definition."))
        {
            kinds[i] = CaptureKind::Definition;
            std::string_view sub = captureName.substr(strlen("local.definition."));
            if (sub == "parameter")
                defKinds[i] = LocalDefinitionKind::Parameter;
            else if (sub == "var")
                defKinds[i] = LocalDefinitionKind::Variable;
            else if (sub == "field")
                defKinds[i] = LocalDefinitionKind::Field;
            else if (sub == "function")
                defKinds[i] = LocalDefinitionKind::Function;
            else if (sub == "method")
                defKinds[i] = LocalDefinitionKind::Method;
            else if (sub == "type")
                defKinds[i] = LocalDefinitionKind::Type;
            else if (sub == "constant")
                defKinds[i] = LocalDefinitionKind::Constant;
            else if (sub == "namespace")
                defKinds[i] = LocalDefinitionKind::Namespace;
            else if (sub == "import")
                defKinds[i] = LocalDefinitionKind::Import;
        }
    }
}

LocalScopeCollector::~LocalScopeCollector() = default;

std::unique_ptr<Scope> LocalScopeCollector::CollectScopes(const std::string& sourceCode,
                                                          angel_lsp::parser::AngelScriptParser& parser) const
{
    TSTree* tree = parser.Parse(sourceCode);
    if (!tree)
        return nullptr;

    TSNode rootNode = ts_tree_root_node(tree);
    std::unique_ptr<Scope> root = CollectScopesFromTree(rootNode, sourceCode);

    ts_tree_delete(tree);
    return root;
}

std::unique_ptr<Scope> LocalScopeCollector::CollectScopesFromTree(TSNode rootNode, const std::string& sourceCode) const
{
    if (!m_localsQuery)
        return nullptr;

    std::vector<RawCapture> captures;

    TSQueryCursor* cursor = angel_lsp::parser::QueryRegistry::GetThreadLocalCursor();
    ts_query_cursor_exec(cursor, m_localsQuery, rootNode);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match))
    {
        for (uint32_t i = 0; i < match.capture_count; ++i)
        {
            uint32_t captureIdx = match.captures[i].index;
            if (captureIdx >= m_captureKinds.size())
                continue;

            CaptureKind kind = m_captureKinds[captureIdx];
            if (kind == CaptureKind::None)
                continue;

            captures.push_back(RawCapture{match.captures[i].node, kind,
                                          kind == CaptureKind::Definition ? m_definitionKinds[captureIdx]
                                                                          : LocalDefinitionKind::Variable});
        }
    }

    return BuildScopeTree(captures, sourceCode);
}

void LocalScopeCollector::DeduplicateCaptures(std::vector<RawCapture>& captures)
{
    std::sort(captures.begin(), captures.end(),
              [](const RawCapture& a, const RawCapture& b)
              {
                  uint32_t aStart = ts_node_start_byte(a.node);
                  uint32_t bStart = ts_node_start_byte(b.node);
                  if (aStart != bStart)
                      return aStart < bStart;
                  return ts_node_end_byte(a.node) > ts_node_end_byte(b.node);
              });

    auto isMoreSpecific = [](LocalDefinitionKind kind)
    { return kind == LocalDefinitionKind::Field || kind == LocalDefinitionKind::Method; };

    std::vector<RawCapture> deduped;
    deduped.reserve(captures.size());
    for (size_t i = 0; i < captures.size(); ++i)
    {
        if (i + 1 < captures.size() && captures[i].kind == CaptureKind::Definition &&
            captures[i + 1].kind == CaptureKind::Definition &&
            ts_node_start_byte(captures[i].node) == ts_node_start_byte(captures[i + 1].node) &&
            ts_node_end_byte(captures[i].node) == ts_node_end_byte(captures[i + 1].node))
        {
            bool secondIsMoreSpecific =
                !isMoreSpecific(captures[i].definitionKind) && isMoreSpecific(captures[i + 1].definitionKind);
            deduped.push_back(secondIsMoreSpecific ? captures[i + 1] : captures[i]);
            ++i;
            continue;
        }
        deduped.push_back(captures[i]);
    }
    captures = std::move(deduped);
}

struct LocalScopeCollector::OpenScope
{
    Scope* scope = nullptr;
    uint32_t endByte = 0;
    uint32_t ownNameStartByte = constants::InvalidByteOffset;
    uint32_t ownNameEndByte = 0;
};

void LocalScopeCollector::ProcessScopeCapture(const RawCapture& capture, std::vector<OpenScope>& stack,
                                              std::unique_ptr<Scope>& root) const
{
    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    auto newScope = std::make_unique<Scope>();
    TSSymbol scopeNodeSymbol = ts_node_symbol(capture.node);
    if (scopeNodeSymbol == m_symLambdaExpression)
    {
        newScope->kind = ScopeKind::Closure;
        newScope->isFunctionScope = true;
    }
    else if (scopeNodeSymbol == m_symFuncDeclaration)
    {
        newScope->kind = ScopeKind::Function;
        newScope->isFunctionScope = true;
    }
    else if (scopeNodeSymbol == m_symClassBody || scopeNodeSymbol == m_symInterfaceBody)
    {
        newScope->kind = ScopeKind::Class;
        newScope->isFunctionScope = false;
    }
    else if (scopeNodeSymbol == m_symNamespaceBody)
    {
        newScope->kind = ScopeKind::Namespace;
        newScope->isFunctionScope = false;
    }
    else if (scopeNodeSymbol == m_symScript)
    {
        newScope->kind = ScopeKind::Global;
        newScope->isFunctionScope = false;
    }
    else
    {
        newScope->kind = ScopeKind::Block;
        newScope->isFunctionScope = false;
    }
    newScope->startLine = startPt.row;
    newScope->startCharacter = startPt.column;
    newScope->endLine = endPt.row;
    newScope->endCharacter = endPt.column;

    uint32_t endByte = ts_node_end_byte(capture.node);
    Scope* scopePtr = nullptr;

    if (stack.empty())
    {
        root = std::move(newScope);
        scopePtr = root.get();
    }
    else
    {
        newScope->parent = stack.back().scope;
        stack.back().scope->children.push_back(std::move(newScope));
        scopePtr = stack.back().scope->children.back().get();
    }

    OpenScope openScope{scopePtr, endByte};
    if (scopeNodeSymbol == m_symFuncDeclaration)
    {
        TSNode ownNameNode = parser::GetChildByField(capture.node, parser::fields::Name);
        if (!ts_node_is_null(ownNameNode))
        {
            openScope.ownNameStartByte = ts_node_start_byte(ownNameNode);
            openScope.ownNameEndByte = ts_node_end_byte(ownNameNode);
        }
    }
    stack.push_back(openScope);
}

void LocalScopeCollector::ProcessDefinitionCapture(const RawCapture& capture, const std::vector<OpenScope>& stack,
                                                   const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node) || !ts_node_is_named(capture.node))
    {
        return;
    }

    Scope* current = stack.back().scope;
    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    uint32_t defStartByte = ts_node_start_byte(capture.node);
    if (defStartByte == stack.back().ownNameStartByte &&
        ts_node_end_byte(capture.node) == stack.back().ownNameEndByte && stack.size() >= 2)
    {
        current = stack[stack.size() - 2].scope;
    }

    LocalDefinition def{GetNodeText(capture.node, sourceCode), capture.definitionKind,
                        SourceRange{startPt.row, startPt.column, endPt.row, endPt.column}};
    def.fullStartLine = startPt.row;
    def.fullStartCharacter = startPt.column;
    def.fullEndLine = endPt.row;
    def.fullEndCharacter = endPt.column;

    if (capture.definitionKind == LocalDefinitionKind::Variable ||
        capture.definitionKind == LocalDefinitionKind::Parameter)
    {
        ReadVariableTypeInfo(capture.node, sourceCode, def);
    }

    current->definitions.push_back(std::move(def));
}

static uint32_t CountArguments(TSNode argsChild)
{
    uint32_t actualArgs = 0;
    TSTreeCursor argCursor = ts_tree_cursor_new(argsChild);
    if (ts_tree_cursor_goto_first_child(&argCursor))
    {
        do
        {
            TSNode ac = ts_tree_cursor_current_node(&argCursor);
            std::string_view act = ts_node_type(ac);
            if (act == "(" || act == ")" || act == "," || act == ":" || act == "comment")
            {
                continue;
            }
            const char* fn = ts_tree_cursor_current_field_name(&argCursor);
            if (fn && std::string_view(fn) == "arg_name")
            {
                continue;
            }
            actualArgs++;
        } while (ts_tree_cursor_goto_next_sibling(&argCursor));
    }
    ts_tree_cursor_delete(&argCursor);
    return actualArgs;
}

static TSNode FindArgumentList(TSNode callNode)
{
    TSNode argsChild = parser::GetChildByField(callNode, parser::fields::Arguments);
    if (!ts_node_is_null(argsChild))
    {
        return argsChild;
    }

    TSTreeCursor searchCursor = ts_tree_cursor_new(callNode);
    if (ts_tree_cursor_goto_first_child(&searchCursor))
    {
        do
        {
            TSNode c = ts_tree_cursor_current_node(&searchCursor);
            if (std::string_view(ts_node_type(c)) == "argument_list")
            {
                argsChild = c;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&searchCursor));
    }
    ts_tree_cursor_delete(&searchCursor);
    return argsChild;
}

void LocalScopeCollector::DetermineCallReferenceInfo(TSNode refNode, TSNode parent, LocalReference& ref)
{
    TSNode walk = refNode;
    TSNode walkParent = parent;
    while (!ts_node_is_null(walkParent))
    {
        std::string_view wpType = ts_node_type(walkParent);
        if (wpType == "call_expression")
        {
            TSNode funcChild = parser::GetChildByField(walkParent, parser::fields::Function);
            if (ts_node_is_null(funcChild))
            {
                TSTreeCursor walkCursor = ts_tree_cursor_new(walkParent);
                if (ts_tree_cursor_goto_first_child(&walkCursor))
                {
                    funcChild = ts_tree_cursor_current_node(&walkCursor);
                }
                ts_tree_cursor_delete(&walkCursor);
            }
            if (!ts_node_is_null(funcChild) &&
                (ts_node_eq(funcChild, walk) || ts_node_start_byte(funcChild) == ts_node_start_byte(walk)))
            {
                ref.isCall = true;
                TSNode argsChild = FindArgumentList(walkParent);
                if (!ts_node_is_null(argsChild))
                {
                    ref.argumentCount = CountArguments(argsChild);
                }
            }
            break;
        }
        if (wpType == "member_expression" || wpType == "scoped_identifier")
        {
            walk = walkParent;
            walkParent = ts_node_parent(walkParent);
        }
        else
        {
            break;
        }
    }
}

static bool IsTypeSpecifierNodeType(std::string_view nodeType)
{
    return nodeType == "datatype" || nodeType == "template_type_list" || nodeType == "base_class_list" ||
           nodeType == "type";
}

static bool IsTypeSpecifierContext(TSNode node)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent))
    {
        return false;
    }
    std::string_view parentType = ts_node_type(parent);
    if (IsTypeSpecifierNodeType(parentType))
    {
        return true;
    }
    if (parentType == "scoped_identifier")
    {
        TSNode grandParent = ts_node_parent(parent);
        if (!ts_node_is_null(grandParent) && IsTypeSpecifierNodeType(ts_node_type(grandParent)))
        {
            return true;
        }
    }
    return false;
}

static bool IsMemberAccessNode(TSNode node, TSNode parent, TSSymbol memberExprSym)
{
    if (ts_node_symbol(parent) != memberExprSym)
    {
        return false;
    }
    TSNode memberField = parser::GetChildByField(parent, parser::fields::Member);
    return ts_node_eq(memberField, node) ||
           (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(node));
}

/**
 * @brief Checks if an anonymous token is in a declaration or modifier context.
 * @param[in] node Anonymous token AST node.
 * @return True if node is a keyword in a declaration/modifier construct.
 */
static bool IsKeywordDeclarationContext(TSNode node)
{
    TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent))
    {
        return true;
    }
    const std::string_view parentType = ts_node_type(parent);
    return parentType == "accessor" || parentType == "func_declaration" || parentType == "class_declaration" ||
           parentType == "interface_declaration" || parentType == "enum_declaration" ||
           parentType == "funcdef_declaration" || parentType == "mixin_declaration" ||
           parentType == "lambda_expression";
}

void LocalScopeCollector::ProcessReferenceCapture(const RawCapture& capture, Scope* current,
                                                  const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node))
    {
        return;
    }
    if (!ts_node_is_named(capture.node))
    {
        const std::string_view nodeType = ts_node_type(capture.node);
        if (nodeType != "function" && nodeType != "get" && nodeType != "set" && nodeType != "shared")
        {
            return;
        }
        if (IsKeywordDeclarationContext(capture.node))
        {
            return;
        }
    }

    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    LocalReference ref{GetNodeText(capture.node, sourceCode), startPt.row, startPt.column, endPt.row, endPt.column};

    TSNode parent = ts_node_parent(capture.node);
    if (!ts_node_is_null(parent))
    {
        ref.isTypeSpecifier = IsTypeSpecifierContext(capture.node);
        ref.isMemberAccess = IsMemberAccessNode(capture.node, parent, m_symMemberExpression);
        DetermineCallReferenceInfo(capture.node, parent, ref);
    }

    current->references.push_back(std::move(ref));
}

std::unique_ptr<Scope> LocalScopeCollector::BuildScopeTree(std::vector<RawCapture>& captures,
                                                           const std::string& sourceCode) const
{
    DeduplicateCaptures(captures);

    std::unique_ptr<Scope> root;
    std::vector<OpenScope> stack;

    for (const RawCapture& capture : captures)
    {
        uint32_t startByte = ts_node_start_byte(capture.node);

        while (!stack.empty() && startByte >= stack.back().endByte)
        {
            stack.pop_back();
        }

        if (capture.kind == CaptureKind::Scope)
        {
            ProcessScopeCapture(capture, stack, root);
            continue;
        }

        if (stack.empty())
        {
            continue;
        }

        if (capture.kind == CaptureKind::Definition)
        {
            ProcessDefinitionCapture(capture, stack, sourceCode);
        }
        else if (capture.kind == CaptureKind::Reference)
        {
            ProcessReferenceCapture(capture, stack.back().scope, sourceCode);
        }
    }

    return root;
}

std::string LocalScopeCollector::GetNodeText(TSNode node, const std::string& sourceCode) const
{
    if (ts_node_is_null(node))
        return "";

    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);

    if (start >= end || end > sourceCode.size())
        return "";

    return sourceCode.substr(start, end - start);
}

void LocalScopeCollector::PopulateTypeRanges(TSNode tNode, const std::string& sourceCode, LocalDefinition& def) const
{
    TSPoint typeStart = ts_node_start_point(tNode);
    TSPoint typeEnd = ts_node_end_point(tNode);
    def.typeStartLine = typeStart.row;
    def.typeStartCharacter = typeStart.column;
    def.typeEndLine = typeEnd.row;
    def.typeEndCharacter = typeEnd.column;

    TSTreeCursor cursor = ts_tree_cursor_new(tNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            std::string_view childType(ts_node_type(child));
            if (childType == "datatype")
            {
                TSPoint dtStart = ts_node_start_point(child);
                TSPoint dtEnd = ts_node_end_point(child);
                def.typeStartLine = dtStart.row;
                def.typeStartCharacter = dtStart.column;
                def.typeEndLine = dtEnd.row;
                def.typeEndCharacter = dtEnd.column;
            }
            else if (childType == "template_type_list")
            {
                TSTreeCursor templateCursor = ts_tree_cursor_new(child);
                if (ts_tree_cursor_goto_first_child(&templateCursor))
                {
                    do
                    {
                        TSNode innerType = ts_tree_cursor_current_node(&templateCursor);
                        if (ts_node_is_named(innerType))
                        {
                            TSPoint argStart = ts_node_start_point(innerType);
                            TSPoint argEnd = ts_node_end_point(innerType);
                            std::string argText = GetNodeText(innerType, sourceCode);
                            def.templateArgPositions.push_back(
                                {CleanBaseType(argText), argStart.row, argStart.column, argEnd.row, argEnd.column});
                        }
                    } while (ts_tree_cursor_goto_next_sibling(&templateCursor));
                }
                ts_tree_cursor_delete(&templateCursor);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void LocalScopeCollector::ReadParameterTypeInfo(TSNode declaratorNode, const std::string& sourceCode,
                                                LocalDefinition& def) const
{
    TSPoint pStart = ts_node_start_point(declaratorNode);
    TSPoint pEnd = ts_node_end_point(declaratorNode);
    def.fullStartLine = pStart.row;
    def.fullStartCharacter = pStart.column;
    def.fullEndLine = pEnd.row;
    def.fullEndCharacter = pEnd.column;

    TSNode paramTypeNode = parser::GetChildByField(declaratorNode, parser::fields::ParamType);
    if (!ts_node_is_null(paramTypeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(paramTypeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(paramTypeNode, sourceCode);
        PopulateTypeRanges(paramTypeNode, sourceCode, def);
    }

    TSTreeCursor cursor = ts_tree_cursor_new(declaratorNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        bool foundEq = false;
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (foundEq)
            {
                def.defaultValue = GetNodeText(child, sourceCode);
                break;
            }
            if (GetNodeText(child, sourceCode) == "=")
            {
                foundEq = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}

void LocalScopeCollector::ReadForeachVariableTypeInfo(TSNode declaratorNode, const std::string& sourceCode,
                                                      LocalDefinition& def) const
{
    TSPoint fStart = ts_node_start_point(declaratorNode);
    TSPoint fEnd = ts_node_end_point(declaratorNode);
    def.fullStartLine = fStart.row;
    def.fullStartCharacter = fStart.column;
    def.fullEndLine = fEnd.row;
    def.fullEndCharacter = fEnd.column;

    TSNode foreachTypeNode = parser::GetChildByField(declaratorNode, parser::fields::Type);
    if (!ts_node_is_null(foreachTypeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(foreachTypeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(foreachTypeNode, sourceCode);
        PopulateTypeRanges(foreachTypeNode, sourceCode, def);
    }
}

static uint32_t CountVariableDeclarators(TSNode declarationNode, TSSymbol declaratorSym)
{
    uint32_t varDeclCount = 0;
    TSTreeCursor declCursor = ts_tree_cursor_new(declarationNode);
    if (ts_tree_cursor_goto_first_child(&declCursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&declCursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == declaratorSym)
            {
                varDeclCount++;
            }
        } while (ts_tree_cursor_goto_next_sibling(&declCursor));
    }
    ts_tree_cursor_delete(&declCursor);
    return varDeclCount;
}

static TSNode FindDeclarationTypeNode(TSNode declarationNode, TSNode declaratorNode)
{
    TSNode typeNode = parser::GetChildByField(declarationNode, parser::fields::VarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(declarationNode, parser::fields::Type);
    }
    if (ts_node_is_null(typeNode))
    {
        TSTreeCursor cursor = ts_tree_cursor_new(declarationNode);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode child = ts_tree_cursor_current_node(&cursor);
                if (ts_node_is_named(child) && !ts_node_eq(child, declaratorNode))
                {
                    typeNode = child;
                    break;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
        }
        ts_tree_cursor_delete(&cursor);
    }
    return typeNode;
}

static TSNode FindDeclaratorValueNode(TSNode declaratorNode, const std::string& sourceCode)
{
    TSNode valueNode = parser::GetChildByField(declaratorNode, parser::fields::Value);
    if (!ts_node_is_null(valueNode))
    {
        return valueNode;
    }

    TSTreeCursor valCursor = ts_tree_cursor_new(declaratorNode);
    if (ts_tree_cursor_goto_first_child(&valCursor))
    {
        bool foundEq = false;
        do
        {
            TSNode child = ts_tree_cursor_current_node(&valCursor);
            if (foundEq)
            {
                valueNode = child;
                break;
            }
            uint32_t start = ts_node_start_byte(child);
            uint32_t end = ts_node_end_byte(child);
            if (start < end && end <= sourceCode.size() && sourceCode.substr(start, end - start) == "=")
            {
                foundEq = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&valCursor));
    }
    ts_tree_cursor_delete(&valCursor);
    return valueNode;
}

void LocalScopeCollector::ReadVariableDeclaratorTypeInfo(TSNode declaratorNode, const std::string& sourceCode,
                                                         LocalDefinition& def) const
{
    TSNode declarationNode = ts_node_parent(declaratorNode);
    if (ts_node_is_null(declarationNode))
        return;

    uint32_t varDeclCount = CountVariableDeclarators(declarationNode, m_symVariableDeclarator);
    TSNode rangeNode = (varDeclCount <= 1) ? declarationNode : declaratorNode;
    TSPoint rStart = ts_node_start_point(rangeNode);
    TSPoint rEnd = ts_node_end_point(rangeNode);
    def.fullStartLine = rStart.row;
    def.fullStartCharacter = rStart.column;
    def.fullEndLine = rEnd.row;
    def.fullEndCharacter = rEnd.column;

    TSNode typeNode = FindDeclarationTypeNode(declarationNode, declaratorNode);
    if (!ts_node_is_null(typeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(typeNode, sourceCode);
        PopulateTypeRanges(typeNode, sourceCode, def);
    }

    TSNode valueNode = FindDeclaratorValueNode(declaratorNode, sourceCode);
    if (!ts_node_is_null(valueNode))
    {
        def.defaultValue = GetNodeText(valueNode, sourceCode);
    }
    def.hasNullInitializer = IsNullInitializer(valueNode);
}

void LocalScopeCollector::ReadVariableTypeInfo(TSNode nameNode, const std::string& sourceCode,
                                               LocalDefinition& def) const
{
    TSNode declaratorNode = ts_node_parent(nameNode);
    if (ts_node_is_null(declaratorNode))
        return;

    if (ts_node_symbol(declaratorNode) == m_symParameter)
    {
        ReadParameterTypeInfo(declaratorNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_symForeachVariable)
    {
        ReadForeachVariableTypeInfo(declaratorNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_symVariableDeclarator)
    {
        ReadVariableDeclaratorTypeInfo(declaratorNode, sourceCode, def);
    }
}
} // namespace angel_lsp::analysis
