#include "analysis/LocalScopeCollector.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeExtraction.h"
#include "parser/QueryRegistry.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"

#include "document/Document.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include <algorithm>
#include <cstring>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
LocalScopeCollector::LocalScopeCollector(angel_lsp::utils::LspLogger* logger)
    : m_logger(logger), m_localsQuery(nullptr), m_gs(parser::GetGrammarSymbols())
{
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
        else if (captureName == "local.string_call_ref")
        {
            kinds[i] = CaptureKind::StringCallRef;
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
    document::TreePtr tree = document::MakeTreePtr(parser.Parse(sourceCode));
    if (!tree)
        return nullptr;

    TSNode rootNode = ts_tree_root_node(tree.get());
    return CollectScopesFromTree(rootNode, sourceCode);
}

LocalScopeCollector::CaptureAncestors
LocalScopeCollector::ResolveCaptureAncestors(CaptureKind kind, LocalDefinitionKind defKind, TSNode node) const
{
    CaptureAncestors result;
    if (kind == CaptureKind::Reference)
    {
        result.parent = parser::GetParent(node);
    }
    else if (kind == CaptureKind::Definition &&
             (defKind == LocalDefinitionKind::Variable || defKind == LocalDefinitionKind::Parameter ||
              defKind == LocalDefinitionKind::Field))
    {
        result.parent = parser::GetParent(node);
        result.grandparent = parser::GetParent(result.parent);
        if (defKind == LocalDefinitionKind::Parameter)
        {
            TSSymbol parentSym = !ts_node_is_null(result.parent) ? ts_node_symbol(result.parent) : 0;
            if (parentSym == m_gs.symLambdaParameterList)
            {
                result.enclosingCallableSym = m_gs.symLambdaExpression;
            }
            else if (!ts_node_is_null(result.grandparent))
            {
                TSNode callable = parser::GetParent(result.grandparent);
                result.enclosingCallableSym = !ts_node_is_null(callable) ? ts_node_symbol(callable) : 0;
            }
        }
    }
    return result;
}

std::unique_ptr<Scope> LocalScopeCollector::CollectScopesFromTree(TSNode rootNode, const std::string& sourceCode) const
{
    if (!m_localsQuery)
        return nullptr;

    std::vector<RawCapture> captures;
    if (sourceCode.size() > 10000)
    {
        captures.reserve(sourceCode.size() / 32);
    }

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

            TSNode node = match.captures[i].node;
            LocalDefinitionKind defKind =
                kind == CaptureKind::Definition ? m_definitionKinds[captureIdx] : LocalDefinitionKind::Variable;

            const auto ancestors = ResolveCaptureAncestors(kind, defKind, node);
            captures.push_back(RawCapture{node, ancestors.parent, ancestors.grandparent, ts_node_start_byte(node),
                                          ts_node_end_byte(node), kind, defKind, ancestors.enclosingCallableSym});
        }
    }

    auto root = BuildScopeTree(captures, sourceCode);
    if (root)
    {
        RegisterVirtualPropertySetParameters(rootNode, root.get(), sourceCode);
        if (ts_node_has_error(rootNode))
        {
            RecoverDeclarationsFromErrorNodes(rootNode, root.get(), sourceCode);
        }
    }
    return root;
}

void LocalScopeCollector::DeduplicateCaptures(std::vector<RawCapture>& captures)
{
    std::sort(captures.begin(), captures.end(),
              [](const RawCapture& a, const RawCapture& b) noexcept
              {
                  if (a.startByte != b.startByte)
                      return a.startByte < b.startByte;
                  return a.endByte > b.endByte;
              });

    auto isMoreSpecific = [](LocalDefinitionKind kind)
    { return kind == LocalDefinitionKind::Field || kind == LocalDefinitionKind::Method; };

    std::vector<RawCapture> deduped;
    deduped.reserve(captures.size());
    for (size_t i = 0; i < captures.size(); ++i)
    {
        if (i + 1 < captures.size() && captures[i].kind == CaptureKind::Definition &&
            captures[i + 1].kind == CaptureKind::Definition && captures[i].startByte == captures[i + 1].startByte &&
            captures[i].endByte == captures[i + 1].endByte)
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
    if (scopeNodeSymbol == m_gs.symLambdaExpression)
    {
        newScope->kind = ScopeKind::Closure;
        newScope->isFunctionScope = true;
    }
    else if (scopeNodeSymbol == m_gs.symFuncDeclaration)
    {
        newScope->kind = ScopeKind::Function;
        newScope->isFunctionScope = true;
    }
    else if (scopeNodeSymbol == m_gs.symClassBody || scopeNodeSymbol == m_gs.symInterfaceBody)
    {
        newScope->kind = ScopeKind::Class;
        newScope->isFunctionScope = false;
    }
    else if (scopeNodeSymbol == m_gs.symNamespaceBody)
    {
        newScope->kind = ScopeKind::Namespace;
        newScope->isFunctionScope = false;
    }
    else if (scopeNodeSymbol == m_gs.symScript)
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

    uint32_t endByte = capture.endByte;
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
    if (scopeNodeSymbol == m_gs.symFuncDeclaration)
    {
        TSNode ownNameNode = parser::GetChildByField(capture.node, parser::GetGrammarSymbols().fieldName);
        if (!ts_node_is_null(ownNameNode))
        {
            openScope.ownNameStartByte = ts_node_start_byte(ownNameNode);
            openScope.ownNameEndByte = ts_node_end_byte(ownNameNode);
        }
    }
    stack.push_back(openScope);
}

/**
 * @brief Checks whether a parameter capture belongs to an executable callable scope.
 * @param kind Active lexical scope kind.
 * @param enclosingSym Pre-resolved symbol of enclosing callable declaration.
 * @param symFunc Function declaration symbol.
 * @param symLambda Lambda expression symbol.
 * @return True if valid parameter definition, false if leaked from signature-only callable.
 */
static bool IsValidParameterCapture(ScopeKind kind, TSSymbol enclosingSym, TSSymbol symFunc,
                                    TSSymbol symLambda) noexcept
{
    if (kind != ScopeKind::Function && kind != ScopeKind::Closure)
    {
        return false;
    }
    return enclosingSym == symFunc || enclosingSym == symLambda;
}

/**
 * @brief Checks if a declared variable type name resolves to a disallowed reserved keyword.
 * @param typeName Type string to inspect.
 * @return True if type name is a disallowed keyword.
 */
static bool IsDisallowedVariableType(std::string_view typeName)
{
    const std::string baseType = CleanBaseType(typeName);
    return IsReservedKeyword(baseType) && !IsPrimitiveTypeName(baseType) && baseType != "auto";
}

void LocalScopeCollector::ProcessDefinitionCapture(const RawCapture& capture, const std::vector<OpenScope>& stack,
                                                   const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node) || !ts_node_is_named(capture.node))
    {
        return;
    }

    Scope* current = stack.back().scope;
    if (capture.definitionKind == LocalDefinitionKind::Parameter &&
        !IsValidParameterCapture(current->kind, capture.enclosingCallableSym, m_gs.symFuncDeclaration,
                                 m_gs.symLambdaExpression))
    {
        return;
    }
    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    uint32_t defStartByte = capture.startByte;
    if (defStartByte == stack.back().ownNameStartByte && capture.endByte == stack.back().ownNameEndByte &&
        stack.size() >= 2)
    {
        current = stack[stack.size() - 2].scope;
    }

    LocalDefinition def{GetNodeText(capture.node, sourceCode), capture.definitionKind,
                        SourceRange{startPt.row, startPt.column, endPt.row, endPt.column}};
    def.fullStartLine = startPt.row;
    def.fullStartCharacter = startPt.column;
    def.fullEndLine = endPt.row;
    def.fullEndCharacter = endPt.column;
    def.startByte = capture.startByte;
    def.endByte = capture.endByte;
    def.visibleFromByte = (capture.definitionKind == LocalDefinitionKind::Variable) ? defStartByte : 0;

    if (capture.definitionKind == LocalDefinitionKind::Variable ||
        capture.definitionKind == LocalDefinitionKind::Parameter ||
        capture.definitionKind == LocalDefinitionKind::Field)
    {
        ReadVariableTypeInfo(capture, sourceCode, def);
        if ((capture.definitionKind == LocalDefinitionKind::Variable ||
             capture.definitionKind == LocalDefinitionKind::Field) &&
            IsDisallowedVariableType(def.typeName))
        {
            return;
        }
    }

    current->definitions.push_back(std::move(def));
}

static TSNode FindArgumentList(TSNode callNode)
{
    TSNode argsChild = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldArguments);
    if (!ts_node_is_null(argsChild))
    {
        return argsChild;
    }

    const auto& syms = parser::GetGrammarSymbols();
    TSTreeCursor searchCursor = ts_tree_cursor_new(callNode);
    if (ts_tree_cursor_goto_first_child(&searchCursor))
    {
        do
        {
            TSNode c = ts_tree_cursor_current_node(&searchCursor);
            if (ts_node_symbol(c) == syms.symArgumentList)
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
    const auto& syms = parser::GetGrammarSymbols();
    TSNode prev = refNode;
    parser::ForEachAncestor(
        parent,
        [&](TSNode walkParent) -> bool
        {
            const TSSymbol wpSym = ts_node_symbol(walkParent);
            if (wpSym == syms.symCallExpression)
            {
                TSNode funcChild = parser::GetChildByField(walkParent, parser::GetGrammarSymbols().fieldFunction);
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
                    (ts_node_eq(funcChild, prev) || ts_node_start_byte(funcChild) == ts_node_start_byte(prev)))
                {
                    ref.isCall = true;
                    TSNode argsChild = FindArgumentList(walkParent);
                    if (!ts_node_is_null(argsChild))
                    {
                        ref.argumentCount = static_cast<uint32_t>(CountCallArguments(argsChild));
                    }
                }
                return false;
            }
            if (wpSym == syms.symMemberAccess || wpSym == syms.symScopedIdentifier)
            {
                prev = walkParent;
                return true;
            }
            return false;
        });
}

static bool IsTypeSpecifierSymbol(TSSymbol sym, const parser::GrammarSymbols& syms) noexcept
{
    return sym == syms.symDatatype || sym == syms.symTemplateTypeList || sym == syms.symBaseClassList ||
           sym == syms.symType;
}

static bool IsTypeSpecifierContext(TSNode parent)
{
    const auto& syms = parser::GetGrammarSymbols();
    bool result = false;
    parser::ForEachAncestor(parent,
                            [&](TSNode cur) -> bool
                            {
                                const TSSymbol sym = ts_node_symbol(cur);
                                if (IsTypeSpecifierSymbol(sym, syms))
                                {
                                    result = true;
                                    return false;
                                }
                                return sym == syms.symScopedIdentifier || sym == syms.symScope;
                            });
    return result;
}

static bool IsMemberAccessNode(TSNode node, TSNode parent, TSSymbol memberExprSym, TSSymbol scopedIdSym)
{
    if (ts_node_symbol(parent) == memberExprSym)
    {
        TSNode memberField = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldMember);
        return ts_node_eq(memberField, node) ||
               (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(node));
    }
    if (ts_node_symbol(parent) == scopedIdSym)
    {
        if (ts_node_child_count(parent) >= 3)
        {
            return true;
        }
        TSNode firstChild = ts_node_named_child(parent, 0);
        return !ts_node_is_null(firstChild) && !ts_node_eq(firstChild, node) &&
               ts_node_start_byte(firstChild) != ts_node_start_byte(node);
    }
    return false;
}

/**
 * @brief Checks if an anonymous token is in a declaration or modifier context.
 * @param[in] parent Parent AST node.
 * @return True if parent is a declaration or modifier construct.
 */
static bool IsKeywordDeclarationContext(TSNode parent, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_null(parent))
    {
        return true;
    }
    const TSSymbol parentSym = ts_node_symbol(parent);
    return parentSym == gs.symAccessor || parentSym == gs.symFuncDeclaration || parentSym == gs.symClassDeclaration ||
           parentSym == gs.symInterfaceDeclaration || parentSym == gs.symEnumDeclaration ||
           parentSym == gs.symFuncdefDeclaration || parentSym == gs.symMixinDeclaration ||
           parentSym == gs.symLambdaExpression || parentSym == gs.symDeclarationModifier ||
           parentSym == gs.symSharedExternalModifier;
}

static bool IsNamedArgumentNode(TSNode node, TSNode parent)
{
    const auto& syms = parser::GetGrammarSymbols();
    if (ts_node_is_null(parent) || ts_node_symbol(parent) != syms.symArgumentList)
    {
        return false;
    }
    TSNode next = ts_node_next_sibling(node);
    while (!ts_node_is_null(next) && ts_node_symbol(next) == syms.symComment)
    {
        next = ts_node_next_sibling(next);
    }
    return !ts_node_is_null(next) && ts_node_symbol(next) == syms.tokColon;
}

static bool ShouldSkipReferenceCapture(TSNode parent, ScopeKind scopeKind, bool hasMetadataInDoc)
{
    const bool isDeclarationScope =
        scopeKind == ScopeKind::Global || scopeKind == ScopeKind::Namespace || scopeKind == ScopeKind::Class;
    if (isDeclarationScope && hasMetadataInDoc)
    {
        const auto& syms = parser::GetGrammarSymbols();
        bool inMetadata = false;
        parser::ForEachAncestor(parent,
                                [&](TSNode curr) -> bool
                                {
                                    if (ts_node_is_error(curr) || ts_node_symbol(curr) == syms.symMetadata)
                                    {
                                        inMetadata = true;
                                        return false;
                                    }
                                    return true;
                                });
        if (inMetadata)
        {
            return true;
        }
    }
    return false;
}

static bool IsAllowedKeywordReference(TSNode node, TSNode parent, const parser::GrammarSymbols& gs)
{
    if (ts_node_is_named(node))
    {
        return true;
    }
    const TSSymbol nodeSym = ts_node_symbol(node);
    if (nodeSym != gs.tokFunction && nodeSym != gs.tokGet && nodeSym != gs.tokSet && nodeSym != gs.tokShared)
    {
        return false;
    }
    return !IsKeywordDeclarationContext(parent, gs);
}

bool LocalScopeCollector::IsDeclarationContextSymbol(TSSymbol sym) const noexcept
{
    return sym == m_gs.symParameter || sym == m_gs.symLambdaParameterList || sym == m_gs.symVariableDeclarator ||
           sym == m_gs.symForeachVariable || sym == m_gs.symClassDeclaration || sym == m_gs.symInterfaceDeclaration ||
           sym == m_gs.symEnumDeclaration || sym == m_gs.symFuncDeclaration || sym == m_gs.symInterfaceMethod ||
           sym == m_gs.symMixinDeclaration || sym == m_gs.symFuncdefDeclaration;
}

void LocalScopeCollector::ProcessReferenceCapture(const RawCapture& capture, Scope* current,
                                                  const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node))
    {
        return;
    }

    TSNode parent = capture.parent;
    if (ts_node_is_null(parent))
    {
        parent = parser::GetParent(capture.node);
    }
    const bool hasMetadataInDoc = (sourceCode.find('[') != std::string::npos);
    if (ShouldSkipReferenceCapture(parent, current->kind, hasMetadataInDoc) ||
        !IsAllowedKeywordReference(capture.node, parent, m_gs))
    {
        return;
    }

    if (!ts_node_is_null(parent) && IsDeclarationContextSymbol(ts_node_symbol(parent)))
    {
        TSNode nameNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldName);
        if (ts_node_eq(nameNode, capture.node) ||
            (!ts_node_is_null(nameNode) && ts_node_start_byte(nameNode) == capture.startByte))
        {
            return;
        }
    }

    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    LocalReference ref;
    ref.name = GetNodeText(capture.node, sourceCode);
    ref.startLine = startPt.row;
    ref.startCharacter = startPt.column;
    ref.endLine = endPt.row;
    ref.endCharacter = endPt.column;
    ref.startByte = capture.startByte;
    ref.endByte = capture.endByte;

    if (!ts_node_is_null(parent))
    {
        ref.isNamedArgument = IsNamedArgumentNode(capture.node, parent);
        ref.isTypeSpecifier = IsTypeSpecifierContext(parent);
        ref.isMemberAccess =
            IsMemberAccessNode(capture.node, parent, m_gs.symMemberExpression, m_gs.symScopedIdentifier);
        DetermineCallReferenceInfo(capture.node, parent, ref);
        PopulateReceiverQualifierRanges(parent, ref, sourceCode);
    }

    current->references.push_back(std::move(ref));
}

void LocalScopeCollector::PopulateReceiverQualifierRanges(TSNode parent, LocalReference& ref,
                                                          const std::string& sourceCode) const
{
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == m_gs.symScopedIdentifier)
    {
        ref.isScopedIdentifier = true;
        ref.qualifierStartByte = ts_node_start_byte(parent);
        ref.qualifierEndByte = ts_node_end_byte(parent);
        TSNode firstChild = ts_node_named_child(parent, 0);
        if (!ts_node_is_null(firstChild))
        {
            ref.receiverStartByte = ts_node_start_byte(firstChild);
            ref.receiverEndByte = ts_node_end_byte(firstChild);
        }
        if (ref.isMemberAccess && ref.qualifierStartByte < ref.startByte && ref.startByte <= sourceCode.size())
        {
            std::string_view qualifierView(sourceCode.data() + ref.qualifierStartByte,
                                           ref.startByte - ref.qualifierStartByte);
            while (qualifierView.ends_with(':') || qualifierView.ends_with(' ') || qualifierView.ends_with('\t'))
            {
                qualifierView.remove_suffix(1);
            }
            ref.scopeQualifier = std::string(qualifierView);
        }
    }
    else if (parentSym == m_gs.symMemberExpression)
    {
        ref.qualifierStartByte = ts_node_start_byte(parent);
        ref.qualifierEndByte = ts_node_end_byte(parent);
        TSNode objNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldObject);
        if (!ts_node_is_null(objNode))
        {
            ref.receiverStartByte = ts_node_start_byte(objNode);
            ref.receiverEndByte = ts_node_end_byte(objNode);
        }
    }
}

void LocalScopeCollector::ProcessStringCallRefCapture(const RawCapture& capture, Scope* current,
                                                      const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node) || !current)
    {
        return;
    }
    std::string_view text = analysis::GetNodeTextView(capture.node, sourceCode);
    text = parser::UnquoteStringView(text);
    if (text.empty() || text.size() > 128)
    {
        return;
    }
    text = analysis::CleanCandidateIdentifier(text);
    if (!analysis::IsValidIdentifierOrQualified(text))
    {
        return;
    }
    TSPoint pt = ts_node_start_point(capture.node);
    current->stringLiteralCallRefs.push_back(
        StringLiteralCallRef{std::string(text), static_cast<uint32_t>(pt.row), static_cast<uint32_t>(pt.column)});
}

std::unique_ptr<Scope> LocalScopeCollector::BuildScopeTree(std::vector<RawCapture>& captures,
                                                           const std::string& sourceCode) const
{
    DeduplicateCaptures(captures);

    std::unique_ptr<Scope> root;
    std::vector<OpenScope> stack;

    for (const RawCapture& capture : captures)
    {
        uint32_t startByte = capture.startByte;

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
        else if (capture.kind == CaptureKind::StringCallRef)
        {
            ProcessStringCallRefCapture(capture, stack.back().scope, sourceCode);
        }
    }

    return root;
}

std::string LocalScopeCollector::GetNodeText(TSNode node, const std::string& sourceCode) const
{
    return parser::GetNodeText(node, sourceCode);
}

void LocalScopeCollector::PopulateTypeRanges(TSNode tNode, const std::string& sourceCode, LocalDefinition& def) const
{
    TSPoint typeStart = ts_node_start_point(tNode);
    TSPoint typeEnd = ts_node_end_point(tNode);
    def.typeStartLine = typeStart.row;
    def.typeStartCharacter = typeStart.column;
    def.typeEndLine = typeEnd.row;
    def.typeEndCharacter = typeEnd.column;

    const auto& syms = parser::GetGrammarSymbols();
    TSTreeCursor cursor = ts_tree_cursor_new(tNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const TSSymbol childSym = ts_node_symbol(child);
            if (childSym == syms.symDatatype)
            {
                TSPoint dtStart = ts_node_start_point(child);
                TSPoint dtEnd = ts_node_end_point(child);
                def.typeStartLine = dtStart.row;
                def.typeStartCharacter = dtStart.column;
                def.typeEndLine = dtEnd.row;
                def.typeEndCharacter = dtEnd.column;
            }
            else if (childSym == syms.symTemplateTypeList)
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

    TSNode paramTypeNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldParamType);
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

    TSNode foreachTypeNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldType);
    if (!ts_node_is_null(foreachTypeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(foreachTypeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(foreachTypeNode, sourceCode);
        PopulateTypeRanges(foreachTypeNode, sourceCode, def);
    }
}

void LocalScopeCollector::ReadLambdaParameterTypeInfo(TSNode nameNode, TSNode declaratorNode,
                                                      const std::string& sourceCode, LocalDefinition& def) const
{
    TSNode startNode = nameNode;
    TSNode typeNode{};

    auto lambdaParams = ExtractLambdaParameters(declaratorNode, sourceCode);
    for (const auto& param : lambdaParams)
    {
        if (ts_node_eq(param.nameNode, nameNode) ||
            (!ts_node_is_null(param.nameNode) && ts_node_start_byte(param.nameNode) == ts_node_start_byte(nameNode)))
        {
            typeNode = param.typeNode;
            if (!ts_node_is_null(param.startNode))
            {
                startNode = param.startNode;
            }
            break;
        }
    }

    TSPoint pStart = ts_node_start_point(startNode);
    TSPoint pEnd = ts_node_end_point(nameNode);
    def.fullStartLine = pStart.row;
    def.fullStartCharacter = pStart.column;
    def.fullEndLine = pEnd.row;
    def.fullEndCharacter = pEnd.column;

    if (!ts_node_is_null(typeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(typeNode, sourceCode);
        PopulateTypeRanges(typeNode, sourceCode, def);
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
    TSNode typeNode = parser::GetChildByField(declarationNode, parser::GetGrammarSymbols().fieldVarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(declarationNode, parser::GetGrammarSymbols().fieldType);
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
    TSNode valueNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldValue);
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

void LocalScopeCollector::ReadVariableDeclaratorTypeInfo(TSNode declaratorNode, TSNode declarationNode,
                                                         const std::string& sourceCode, LocalDefinition& def) const
{
    if (ts_node_is_null(declarationNode))
    {
        declarationNode = parser::GetParent(declaratorNode);
    }
    if (ts_node_is_null(declarationNode))
        return;

    uint32_t varDeclCount = CountVariableDeclarators(declarationNode, m_gs.symVariableDeclarator);
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

void LocalScopeCollector::ReadVariableTypeInfo(const RawCapture& capture, const std::string& sourceCode,
                                               LocalDefinition& def) const
{
    TSNode declaratorNode = capture.parent;
    if (ts_node_is_null(declaratorNode))
    {
        declaratorNode = parser::GetParent(capture.node);
    }
    if (ts_node_is_null(declaratorNode))
        return;

    if (ts_node_symbol(declaratorNode) == m_gs.symParameter)
    {
        ReadParameterTypeInfo(declaratorNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_gs.symForeachVariable)
    {
        ReadForeachVariableTypeInfo(declaratorNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_gs.symVariableDeclarator)
    {
        TSNode declarationNode = capture.grandparent;
        if (ts_node_is_null(declarationNode))
        {
            declarationNode = parser::GetParent(declaratorNode);
        }
        ReadVariableDeclaratorTypeInfo(declaratorNode, declarationNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_gs.symLambdaParameterList)
    {
        ReadLambdaParameterTypeInfo(capture.node, declaratorNode, sourceCode, def);
        return;
    }
    if (ts_node_symbol(declaratorNode) == m_gs.symVirtualProperty)
    {
        ReadVirtualPropertyTypeInfo(declaratorNode, sourceCode, def);
    }
}

void LocalScopeCollector::ReadVirtualPropertyTypeInfo(TSNode propertyNode, const std::string& sourceCode,
                                                      LocalDefinition& def) const
{
    TSPoint rStart = ts_node_start_point(propertyNode);
    TSPoint rEnd = ts_node_end_point(propertyNode);
    def.fullStartLine = rStart.row;
    def.fullStartCharacter = rStart.column;
    def.fullEndLine = rEnd.row;
    def.fullEndCharacter = rEnd.column;

    TSNode typeNode = parser::GetChildByField(propertyNode, parser::GetGrammarSymbols().fieldPropType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(propertyNode, parser::GetGrammarSymbols().fieldType);
    }
    if (!ts_node_is_null(typeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.typeKind = typeInfo.kind;
        def.typeName = GetNodeText(typeNode, sourceCode);
        PopulateTypeRanges(typeNode, sourceCode, def);
    }
}

namespace
{
Scope* FindInnermostScopeMut(Scope* root, uint32_t line, uint32_t character)
{
    if (!root)
        return nullptr;
    for (auto& child : root->children)
    {
        if (line > child->startLine || (line == child->startLine && character >= child->startCharacter))
        {
            if (line < child->endLine || (line == child->endLine && character <= child->endCharacter))
            {
                Scope* inner = FindInnermostScopeMut(child.get(), line, character);
                return inner ? inner : child.get();
            }
        }
    }
    return root;
}

void InsertRecoveredDefinition(Scope* targetScope, LocalDefinition&& def)
{
    if (!targetScope)
        return;
    for (const auto& existing : targetScope->definitions)
    {
        if (existing.name == def.name)
            return;
    }
    targetScope->definitions.push_back(std::move(def));
}

bool ExtractAssignmentLeftVar(TSNode leftNode, const parser::GrammarSymbols& gs, std::string_view sourceCode,
                              std::string& outName, bool& outIsHandle, TSNode& outIdentNode)
{
    TSSymbol sym = ts_node_symbol(leftNode);
    if (sym == gs.symIdentifier)
    {
        outName = parser::GetNodeText(leftNode, sourceCode);
        outIdentNode = leftNode;
        return !outName.empty();
    }
    if (sym == gs.symScopedIdentifier)
    {
        TSNode id = parser::GetChildByField(leftNode, parser::GetGrammarSymbols().fieldName);
        if (ts_node_is_null(id))
        {
            uint32_t count = ts_node_child_count(leftNode);
            id = (count > 0) ? ts_node_child(leftNode, count - 1) : TSNode{};
        }
        outName = parser::GetNodeText(id, sourceCode);
        outIdentNode = id;
        return !outName.empty();
    }
    if (sym == gs.symUnaryExpression)
    {
        std::string_view text = parser::GetNodeTextView(leftNode, sourceCode);
        if (text.starts_with('@'))
        {
            outIsHandle = true;
            TSNode operand = parser::GetChildByField(leftNode, parser::GetGrammarSymbols().fieldOperand);
            if (ts_node_is_null(operand))
                operand = ts_node_child(leftNode, 1);
            return ExtractAssignmentLeftVar(operand, gs, sourceCode, outName, outIsHandle, outIdentNode);
        }
    }
    return false;
}

void RegisterRecoveredVariable(Scope* rootScope, TSNode identNode, std::string varName, std::string typeName,
                               bool isHandle)
{
    if (isHandle && !typeName.ends_with('@'))
    {
        typeName += "@";
    }
    TSPoint pt = ts_node_start_point(identNode);
    TSPoint endPt = ts_node_end_point(identNode);
    LocalDefinition def{
        std::move(varName), LocalDefinitionKind::Variable, {pt.row, pt.column, endPt.row, endPt.column}};
    def.fullStartLine = pt.row;
    def.fullStartCharacter = pt.column;
    def.fullEndLine = endPt.row;
    def.fullEndCharacter = endPt.column;
    def.typeName = std::move(typeName);
    def.isHandleType = isHandle;
    def.visibleFromByte = ts_node_start_byte(identNode);
    Scope* target = FindInnermostScopeMut(rootScope, pt.row, pt.column);
    InsertRecoveredDefinition(target, std::move(def));
}

bool TryRecoverSplitIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
                                  const parser::GrammarSymbols& gs)
{
    TSNode nextStmt = ts_node_next_sibling(node);
    if (ts_node_is_null(nextStmt) || ts_node_symbol(nextStmt) != gs.symExpressionStatement)
    {
        return false;
    }
    TSNode cons = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldConsequence);
    if (ts_node_is_null(cons))
    {
        uint32_t count = ts_node_child_count(node);
        cons = (count > 0) ? ts_node_child(node, count - 1) : TSNode{};
    }
    if (ts_node_is_null(cons) || ts_node_symbol(cons) != gs.symExpressionStatement)
    {
        return false;
    }
    std::string_view typeView = parser::GetNodeTextView(ts_node_child(cons, 0), sourceCode);
    while (!typeView.empty() && (typeView.back() == ' ' || typeView.back() == '\t' || typeView.back() == ';'))
    {
        typeView.remove_suffix(1);
    }
    if (typeView.empty())
    {
        return false;
    }
    TSNode assignExpr = ts_node_child(nextStmt, 0);
    if (ts_node_is_null(assignExpr) || ts_node_symbol(assignExpr) != gs.symAssignmentExpression)
    {
        return false;
    }
    TSNode left = parser::GetChildByField(assignExpr, parser::GetGrammarSymbols().fieldLeft);
    std::string varName;
    bool isHandle = false;
    TSNode identNode = TSNode{};
    if (ExtractAssignmentLeftVar(left, gs, sourceCode, varName, isHandle, identNode))
    {
        RegisterRecoveredVariable(rootScope, identNode, std::move(varName), std::string(typeView), isHandle);
        return true;
    }
    return false;
}

void TryRecoverErrorIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
                                  const parser::GrammarSymbols& gs)
{
    TSTreeCursor cursor = ts_tree_cursor_new(node);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return;
    }
    std::string_view errTypeView;
    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_error(child))
        {
            errTypeView = parser::GetNodeTextView(child, sourceCode);
            while (!errTypeView.empty() && (errTypeView.back() == ' ' || errTypeView.back() == '\t'))
            {
                errTypeView.remove_suffix(1);
            }
            break;
        }
    } while (ts_tree_cursor_goto_next_sibling(&cursor));
    ts_tree_cursor_delete(&cursor);

    if (errTypeView.empty())
    {
        return;
    }
    std::string errTypeName(errTypeView);
    TSNode cons = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldConsequence);
    if (ts_node_is_null(cons) || ts_node_symbol(cons) != gs.symExpressionStatement)
    {
        return;
    }
    TSNode assignExpr = ts_node_child(cons, 0);
    if (ts_node_is_null(assignExpr) || ts_node_symbol(assignExpr) != gs.symAssignmentExpression)
    {
        return;
    }
    TSNode left = parser::GetChildByField(assignExpr, parser::GetGrammarSymbols().fieldLeft);
    std::string varName;
    bool isHandle = false;
    TSNode identNode = TSNode{};
    if (ExtractAssignmentLeftVar(left, gs, sourceCode, varName, isHandle, identNode))
    {
        RegisterRecoveredVariable(rootScope, identNode, std::move(varName), std::move(errTypeName), isHandle);
    }
}

void TryRecoverUnbracedIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
                                     const parser::GrammarSymbols& gs)
{
    if (TryRecoverSplitIfDeclaration(node, rootScope, sourceCode, gs))
    {
        return;
    }
    TryRecoverErrorIfDeclaration(node, rootScope, sourceCode, gs);
}

struct VirtualPropertyContext
{
    const std::string& propTypeStr;
    const TypeExtractionResult& typeInfo;
    Scope* root = nullptr;
    const parser::GrammarSymbols& gs;
    std::string_view sourceCode;
};

void ProcessVirtualPropertySetAccessor(TSNode accNode, const VirtualPropertyContext& ctx)
{
    TSNode kindNode = parser::GetChildByField(accNode, ctx.gs.fieldKind);
    if (ts_node_is_null(kindNode))
    {
        return;
    }
    const bool isSet =
        (ts_node_symbol(kindNode) == ctx.gs.tokSet || parser::GetNodeText(kindNode, ctx.sourceCode) == "set");
    if (!isSet)
    {
        return;
    }
    TSNode bodyNode = parser::GetChildByField(accNode, ctx.gs.fieldBody);
    if (ts_node_is_null(bodyNode))
    {
        return;
    }
    TSPoint pt = ts_node_start_point(bodyNode);
    Scope* setScope = FindInnermostScopeMut(ctx.root, pt.row, pt.column);
    if (!setScope)
    {
        return;
    }
    for (const auto& existing : setScope->definitions)
    {
        if (existing.name == "value")
        {
            return;
        }
    }
    LocalDefinition def;
    def.name = "value";
    def.kind = LocalDefinitionKind::Parameter;
    def.typeName = ctx.propTypeStr;
    def.typeKind = ctx.typeInfo.kind;
    def.isHandleType = ctx.typeInfo.isHandle;
    def.visibleFromByte = 0;
    TSPoint kStart = ts_node_start_point(kindNode);
    TSPoint kEnd = ts_node_end_point(kindNode);
    def.startLine = kStart.row;
    def.startCharacter = kStart.column;
    def.endLine = kEnd.row;
    def.endCharacter = kEnd.column;
    def.fullStartLine = kStart.row;
    def.fullStartCharacter = kStart.column;
    def.fullEndLine = kEnd.row;
    def.fullEndCharacter = kEnd.column;
    setScope->definitions.push_back(std::move(def));
}

void ProcessVirtualPropertyNode(TSNode propNode, Scope* root, const parser::GrammarSymbols& gs,
                                std::string_view sourceCode)
{
    TSNode propTypeNode = parser::GetChildByField(propNode, gs.fieldPropType);
    if (ts_node_is_null(propTypeNode))
    {
        propTypeNode = parser::GetChildByField(propNode, gs.fieldType);
    }
    if (ts_node_is_null(propTypeNode))
    {
        return;
    }
    std::string propTypeStr = parser::GetNodeText(propTypeNode, sourceCode);
    TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(propTypeNode, sourceCode);
    VirtualPropertyContext ctx{propTypeStr, typeInfo, root, gs, sourceCode};

    TSTreeCursor cursor = ts_tree_cursor_new(propNode);
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (ts_node_is_named(child) && ts_node_symbol(child) == gs.symAccessor)
            {
                ProcessVirtualPropertySetAccessor(child, ctx);
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
}
} // namespace

void LocalScopeCollector::RecoverDeclarationsFromErrorNodes(TSNode rootNode, Scope* rootScope,
                                                            const std::string& sourceCode) const
{
    if (!ts_node_has_error(rootNode) || !rootScope)
        return;

    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    bool hasChild = true;

    while (hasChild)
    {
        TSNode current = ts_tree_cursor_current_node(&cursor);
        TSSymbol sym = ts_node_symbol(current);

        if (sym == m_gs.symIfStatement || sym == m_gs.symWhileStatement || sym == m_gs.symForStatement)
        {
            TryRecoverUnbracedIfDeclaration(current, rootScope, sourceCode, m_gs);
        }

        if (sym != m_gs.symComment && ts_node_has_error(current) && ts_tree_cursor_goto_first_child(&cursor))
        {
            continue;
        }

        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            continue;
        }

        bool ascended = false;
        while (ts_tree_cursor_goto_parent(&cursor))
        {
            if (ts_tree_cursor_goto_next_sibling(&cursor))
            {
                ascended = true;
                break;
            }
        }
        if (!ascended)
            break;
    }

    ts_tree_cursor_delete(&cursor);
}

void LocalScopeCollector::RegisterVirtualPropertySetParameters(TSNode rootNode, Scope* rootScope,
                                                               const std::string& sourceCode) const
{
    if (!rootScope || ts_node_is_null(rootNode))
    {
        return;
    }
    static constexpr size_t kMaxDepth = 64;
    TSTreeCursor cursor = ts_tree_cursor_new(rootNode);
    size_t depth = 0;
    bool visiting = true;

    while (visiting)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (ts_node_is_named(node) && ts_node_symbol(node) == m_gs.symVirtualProperty)
        {
            ProcessVirtualPropertyNode(node, rootScope, m_gs, sourceCode);
        }

        if (depth < kMaxDepth && ts_tree_cursor_goto_first_child(&cursor))
        {
            ++depth;
            continue;
        }

        while (!ts_tree_cursor_goto_next_sibling(&cursor))
        {
            if (!ts_tree_cursor_goto_parent(&cursor))
            {
                visiting = false;
                break;
            }
            --depth;
        }
    }
    ts_tree_cursor_delete(&cursor);
}
} // namespace angel_lsp::analysis
