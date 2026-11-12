#include "analysis/index/LocalScopeCollector.h"

#include "analysis/index/CallArgumentFacts.h"
#include "analysis/index/ReferenceReceiverIndex.h"
#include "analysis/index/TypeExtraction.h"
#include "analysis/types/TypeSanitization.h"
#include "document/Document.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/GrammarSymbols.h"
#include "parser/Keywords.h"
#include "parser/Primitives.h"
#include "parser/QueryRegistry.h"
#include "parser/SyntaxTree.h"
#include "parser/queries/BuiltQueries.h"
#include "spdlog/fmt/fmt.h"
#include "utils/Constants.h"
#include "utils/LspLogger.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>

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
    parser::TreePtr tree = parser::MakeTreePtr(parser.Parse(sourceCode));
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
    else if (kind == CaptureKind::Definition)
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

void LocalScopeCollector::CollectRawCaptures(TSNode rootNode, std::vector<RawCapture>& captures) const
{
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
}

static LocalScopeCollector::DocumentContext BuildDocumentContext(const NodeIndex& index,
                                                                 const parser::GrammarSymbols& grammar)
{
    LocalScopeCollector::DocumentContext context;
    context.treeHasError = ts_node_has_error(index.Root());
    context.hasMetadata = !index.Nodes(grammar.symMetadata).empty();

    const std::array symbols{grammar.symMetadata, grammar.symError};
    index.ForEachNodeOrdered(symbols,
                             [&](TSNode node)
                             {
                                 if (ts_node_is_missing(node))
                                 {
                                     return;
                                 }

                                 const uint32_t start = ts_node_start_byte(node);
                                 const uint32_t end = ts_node_end_byte(node);
                                 auto& spans = context.metadataOrErrorSpans;
                                 if (!spans.empty() && start <= spans.back().endByte)
                                 {
                                     spans.back().endByte = std::max(end, spans.back().endByte);
                                 }
                                 else
                                 {
                                     spans.push_back({start, end});
                                 }
                             });
    return context;
}

std::unique_ptr<Scope> LocalScopeCollector::CollectScopesFromIndex(const NodeIndex& nodeIndex,
                                                                   const std::string& sourceCode) const
{
    return CollectScopesFromTree(nodeIndex.Root(), sourceCode, &nodeIndex);
}

std::unique_ptr<Scope> LocalScopeCollector::CollectScopesFromTree(TSNode rootNode, const std::string& sourceCode,
                                                                  const NodeIndex* nodeIndex) const
{
    if (!m_localsQuery || ts_node_is_null(rootNode))
    {
        return nullptr;
    }

    std::optional<NodeIndex> localIndex;
    if (!nodeIndex)
    {
        nodeIndex = &localIndex.emplace(rootNode);
    }

    std::vector<RawCapture> captures;
    if (sourceCode.size() > 10000)
    {
        captures.reserve(sourceCode.size() / 32);
    }
    CollectRawCaptures(rootNode, captures);

    DocumentContext docCtx = BuildDocumentContext(*nodeIndex, m_gs);
    auto root = BuildScopeTree(captures, sourceCode, docCtx);
    if (root)
    {
        RegisterVirtualPropertySetParameters(*nodeIndex, root.get(), sourceCode);
        if (docCtx.treeHasError)
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

/**
 * @brief Checks if a function declaration has method qualifiers preventing it from being treated as a variable.
 */
static bool HasDisqualifyingMethodQualifiers(TSNode funcNode, TSNode paramsNode, std::string_view sourceCode) noexcept
{
    std::string_view fullText = parser::NodeText(funcNode, sourceCode);
    if (fullText.find("override") != std::string_view::npos || fullText.find("final") != std::string_view::npos ||
        fullText.find("delete") != std::string_view::npos || fullText.find("abstract") != std::string_view::npos ||
        fullText.find("external") != std::string_view::npos || fullText.find("import") != std::string_view::npos)
    {
        return true;
    }
    if (!ts_node_is_null(paramsNode))
    {
        const auto& gs = parser::GetGrammarSymbols();
        if (!ts_node_is_null(parser::FindChildWithSymbol(funcNode, gs.symFuncAttributes)) ||
            !ts_node_is_null(parser::FindChildWithSymbol(funcNode, gs.tokConst)))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if a function declaration returns a reference or handle.
 */
static bool HasReferenceOrHandleReturnType(TSNode funcNode, TSNode typeNode, const parser::GrammarSymbols& gs,
                                           std::string_view sourceCode) noexcept
{
    std::string_view retText = parser::NodeText(typeNode, sourceCode);
    if (retText.empty() || retText == "void" || IsReferenceTypeView(retText) || IsHandleTypeView(retText))
    {
        return true;
    }
    TSNode nameNode = parser::GetChildByField(funcNode, gs.fieldName);
    if (!ts_node_is_null(nameNode))
    {
        uint32_t tEnd = ts_node_end_byte(typeNode);
        uint32_t nStart = ts_node_start_byte(nameNode);
        if (tEnd < nStart && nStart <= sourceCode.size())
        {
            std::string_view between = sourceCode.substr(tEnd, nStart - tEnd);
            if (IsReferenceTypeView(between) || IsHandleTypeView(between))
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Checks if a function declaration is an empty-parentheses bodyless declaration (variable declaration).
 * @param funcNode Function declaration AST node.
 * @param gs Grammar symbols.
 * @param sourceCode Document source text.
 * @return True if empty-parentheses bodyless declaration.
 */
static bool IsEmptyParenFuncDeclaration(TSNode funcNode, const parser::GrammarSymbols& gs,
                                        std::string_view sourceCode) noexcept
{
    if (ts_node_is_null(funcNode) || ts_node_symbol(funcNode) != gs.symFuncDeclaration)
    {
        return false;
    }
    TSNode parent = parser::GetParent(funcNode);
    if (!ts_node_is_null(parent) && ts_node_symbol(parent) == gs.symInterfaceBody)
    {
        return false;
    }
    if (!ts_node_is_null(parser::GetChildByField(funcNode, gs.fieldBody)))
    {
        return false;
    }
    TSNode paramsNode = parser::GetChildByField(funcNode, gs.fieldParameters);
    if (!ts_node_is_null(paramsNode) && ts_node_named_child_count(paramsNode) > 0)
    {
        return false;
    }
    TSNode typeNode = parser::GetChildByField(funcNode, gs.fieldReturnType);
    if (ts_node_is_null(typeNode))
    {
        return false;
    }
    if (HasReferenceOrHandleReturnType(funcNode, typeNode, gs, sourceCode))
    {
        return false;
    }
    return !HasDisqualifyingMethodQualifiers(funcNode, paramsNode, sourceCode);
}

/**
 * @brief Resolves ScopeKind and function scope flag for a scope AST node.
 */
static std::optional<std::pair<ScopeKind, bool>> DetermineScopeKind(TSNode node, const parser::GrammarSymbols& gs,
                                                                    std::string_view sourceCode) noexcept
{
    TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symLambdaExpression)
    {
        return std::make_pair(ScopeKind::Closure, true);
    }
    if (sym == gs.symFuncDeclaration)
    {
        if (IsEmptyParenFuncDeclaration(node, gs, sourceCode))
        {
            return std::nullopt;
        }
        return std::make_pair(ScopeKind::Function, true);
    }
    if (sym == gs.symClassBody || sym == gs.symInterfaceBody)
    {
        return std::make_pair(ScopeKind::Class, false);
    }
    if (sym == gs.symNamespaceBody)
    {
        return std::make_pair(ScopeKind::Namespace, false);
    }
    if (sym == gs.symScript)
    {
        return std::make_pair(ScopeKind::Global, false);
    }
    return std::make_pair(ScopeKind::Block, false);
}

static void UpdateClassScopeContext(Scope* scopePtr, TSNode node, const parser::GrammarSymbols& gs,
                                    std::string_view sourceCode)
{
    TSNode parent = parser::GetParent(node);
    if (!ts_node_is_null(parent))
    {
        TSNode nameNode = parser::GetChildByField(parent, gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            std::string clsName(parser::NodeText(nameNode, sourceCode));
            auto newCtx = std::make_shared<ScopeEnclosingContext>(
                scopePtr->enclosingContext ? *scopePtr->enclosingContext : ScopeEnclosingContext{});
            newCtx->enclosingClass = utils::JoinScopedName(newCtx->enclosingNamespace, clsName);
            scopePtr->enclosingContext = std::move(newCtx);
        }
    }
}

static void UpdateNamespaceScopeContext(Scope* scopePtr, TSNode node, const parser::GrammarSymbols& gs,
                                        std::string_view sourceCode)
{
    TSNode parent = parser::GetParent(node);
    if (!ts_node_is_null(parent))
    {
        TSNode nameNode = parser::GetChildByField(parent, gs.fieldName);
        if (!ts_node_is_null(nameNode))
        {
            std::string nsName(parser::NodeText(nameNode, sourceCode));
            auto newCtx = std::make_shared<ScopeEnclosingContext>(
                scopePtr->enclosingContext ? *scopePtr->enclosingContext : ScopeEnclosingContext{});
            newCtx->enclosingNamespace = utils::JoinScopedName(newCtx->enclosingNamespace, nsName);
            scopePtr->enclosingContext = std::move(newCtx);
        }
    }
}

static bool CheckMethodIsConst(TSNode funcNode, const parser::GrammarSymbols& gs) noexcept
{
    TSTreeCursor cursor = ts_tree_cursor_new(funcNode);
    bool isConst = false;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            const TSSymbol childSym = ts_node_symbol(child);
            if (childSym == gs.tokConst ||
                (childSym == gs.symFuncAttributes && !ts_node_is_null(parser::FindChildWithSymbol(child, gs.tokConst))))
            {
                isConst = true;
                break;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return isConst;
}

static void UpdateFuncScopeContext(LocalScopeCollector::OpenScope& openScope, TSNode node,
                                   const parser::GrammarSymbols& gs, std::string_view sourceCode)
{
    TSNode ownNameNode = parser::GetChildByField(node, gs.fieldName);
    if (!ts_node_is_null(ownNameNode))
    {
        openScope.ownNameStartByte = ts_node_start_byte(ownNameNode);
        openScope.ownNameEndByte = ts_node_end_byte(ownNameNode);

        std::string fnName(parser::NodeText(ownNameNode, sourceCode));
        std::string_view parentPrefix = !openScope.scope->GetEnclosingClass().empty()
                                            ? openScope.scope->GetEnclosingClass()
                                            : openScope.scope->GetEnclosingNamespace();
        auto newCtx = std::make_shared<ScopeEnclosingContext>(
            openScope.scope->enclosingContext ? *openScope.scope->enclosingContext : ScopeEnclosingContext{});
        newCtx->caller = utils::JoinScopedName(parentPrefix, fnName);
        openScope.scope->enclosingContext = std::move(newCtx);
    }
    openScope.scope->isConstMethod = CheckMethodIsConst(node, gs);
}

static void UpdateScopeContextAndOpen(LocalScopeCollector::OpenScope& openScope, TSNode node,
                                      const parser::GrammarSymbols& gs, std::string_view sourceCode)
{
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symClassBody || sym == gs.symInterfaceBody)
    {
        UpdateClassScopeContext(openScope.scope, node, gs, sourceCode);
    }
    else if (sym == gs.symNamespaceBody)
    {
        UpdateNamespaceScopeContext(openScope.scope, node, gs, sourceCode);
    }
    else if (sym == gs.symFuncDeclaration)
    {
        UpdateFuncScopeContext(openScope, node, gs, sourceCode);
    }
}

void LocalScopeCollector::ProcessScopeCapture(const RawCapture& capture, std::vector<OpenScope>& stack,
                                              std::unique_ptr<Scope>& root, const std::string& sourceCode) const
{
    auto kindInfo = DetermineScopeKind(capture.node, m_gs, sourceCode);
    if (!kindInfo)
    {
        return;
    }

    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    auto newScope = std::make_unique<Scope>();
    newScope->kind = kindInfo->first;
    newScope->isFunctionScope = kindInfo->second;
    newScope->startLine = startPt.row;
    newScope->startCharacter = startPt.column;
    newScope->endLine = endPt.row;
    newScope->endCharacter = endPt.column;

    Scope* scopePtr = nullptr;
    if (stack.empty())
    {
        newScope->enclosingContext = std::make_shared<ScopeEnclosingContext>();
        root = std::move(newScope);
        scopePtr = root.get();
    }
    else
    {
        newScope->parent = stack.back().scope;
        newScope->enclosingContext = stack.back().scope->enclosingContext;
        newScope->isConstMethod = stack.back().scope->isConstMethod;
        stack.back().scope->children.push_back(std::move(newScope));
        scopePtr = stack.back().scope->children.back().get();
    }

    OpenScope openScope{scopePtr, capture.endByte};
    UpdateScopeContextAndOpen(openScope, capture.node, m_gs, sourceCode);
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
    return parser::keywords::IsReserved(baseType) && !parser::primitives::IsPrimitive(baseType) && baseType != "auto";
}

/**
 * @brief Resolves effective local definition kind, converting empty-parentheses func declarations to variable/field.
 * @param rawKind Query capture definition kind.
 * @param parentNode Parent AST node.
 * @param currentScopeKind Active enclosing scope kind.
 * @param sourceCode Document source text.
 * @return Effective local definition kind.
 */
static LocalDefinitionKind DetermineDefinitionKind(LocalDefinitionKind rawKind, TSNode parentNode,
                                                   ScopeKind currentScopeKind, std::string_view sourceCode) noexcept
{
    if ((rawKind == LocalDefinitionKind::Function || rawKind == LocalDefinitionKind::Method) &&
        IsEmptyParenFuncDeclaration(parentNode, parser::GetGrammarSymbols(), sourceCode))
    {
        return (currentScopeKind == ScopeKind::Class) ? LocalDefinitionKind::Field : LocalDefinitionKind::Variable;
    }
    return rawKind;
}

/**
 * @brief Computes byte offset from which a definition is visible within its enclosing scope.
 * @param kind Definition kind.
 * @param scopeKind Enclosing scope kind.
 * @param defStartByte Declaration start byte.
 * @return Visible-from byte offset.
 */
static uint32_t CalculateVisibleFromByte(LocalDefinitionKind kind, ScopeKind scopeKind, uint32_t defStartByte) noexcept
{
    if (kind != LocalDefinitionKind::Variable)
    {
        return 0;
    }
    if (scopeKind == ScopeKind::Global || scopeKind == ScopeKind::Namespace || scopeKind == ScopeKind::Class)
    {
        return 0;
    }
    return defStartByte;
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

    LocalDefinitionKind defKind =
        DetermineDefinitionKind(capture.definitionKind, capture.parent, current->kind, sourceCode);
    TSPoint startPt = ts_node_start_point(capture.node);
    TSPoint endPt = ts_node_end_point(capture.node);

    uint32_t defStartByte = capture.startByte;
    if (defStartByte == stack.back().ownNameStartByte && capture.endByte == stack.back().ownNameEndByte &&
        stack.size() >= 2)
    {
        current = stack[stack.size() - 2].scope;
    }

    LocalDefinition def{std::string(parser::NodeText(capture.node, sourceCode)), defKind,
                        SourceRange{startPt.row, startPt.column, endPt.row, endPt.column}};
    def.fullStartLine = startPt.row;
    def.fullStartCharacter = startPt.column;
    def.fullEndLine = endPt.row;
    def.fullEndCharacter = endPt.column;
    def.startByte = capture.startByte;
    def.endByte = capture.endByte;
    def.visibleFromByte = CalculateVisibleFromByte(defKind, current->kind, defStartByte);

    if (defKind == LocalDefinitionKind::Variable || defKind == LocalDefinitionKind::Parameter ||
        defKind == LocalDefinitionKind::Field)
    {
        ReadVariableTypeInfo(capture, sourceCode, def);
        if ((defKind == LocalDefinitionKind::Variable || defKind == LocalDefinitionKind::Field) &&
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

static void PopulateCallArgs(TSNode callNode, LocalReference& ref)
{
    ref.isCall = true;
    TSNode argsChild = FindArgumentList(callNode);
    if (!ts_node_is_null(argsChild))
    {
        ref.argumentCount = static_cast<uint32_t>(CountCallArguments(argsChild));
    }
}

static bool IsCallCalleeNode(TSNode parent, TSSymbol parentSym, TSNode refNode,
                             const parser::GrammarSymbols& syms) noexcept
{
    if (parentSym == syms.symMemberExpression || parentSym == syms.symMemberAccess)
    {
        TSNode memberField = parser::GetChildByField(parent, syms.fieldMember);
        return ts_node_eq(memberField, refNode) ||
               (!ts_node_is_null(memberField) && ts_node_start_byte(memberField) == ts_node_start_byte(refNode));
    }
    if (parentSym == syms.symScopedIdentifier)
    {
        TSNode nameField = parser::GetChildByField(parent, syms.fieldName);
        if (ts_node_is_null(nameField))
        {
            const uint32_t count = ts_node_named_child_count(parent);
            if (count > 0)
            {
                nameField = ts_node_named_child(parent, count - 1);
            }
        }
        return ts_node_eq(nameField, refNode) ||
               (!ts_node_is_null(nameField) && ts_node_start_byte(nameField) == ts_node_start_byte(refNode));
    }
    return false;
}

void LocalScopeCollector::DetermineCallReferenceInfo(TSNode refNode, TSNode parent, TSNode grandparent,
                                                     LocalReference& ref)
{
    if (ts_node_is_null(parent))
    {
        return;
    }

    const auto& syms = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);

    if (parentSym == syms.symCallExpression)
    {
        TSNode funcChild = parser::GetChildByField(parent, syms.fieldFunction);
        if (ts_node_eq(funcChild, refNode) ||
            (!ts_node_is_null(funcChild) && ts_node_start_byte(funcChild) == ts_node_start_byte(refNode)))
        {
            PopulateCallArgs(parent, ref);
        }
        return;
    }

    if (ts_node_is_null(grandparent) || ts_node_symbol(grandparent) != syms.symCallExpression)
    {
        return;
    }

    TSNode callFunc = parser::GetChildByField(grandparent, syms.fieldFunction);
    if (!ts_node_eq(callFunc, parent) &&
        (ts_node_is_null(callFunc) || ts_node_start_byte(callFunc) != ts_node_start_byte(parent)))
    {
        return;
    }

    if (IsCallCalleeNode(parent, parentSym, refNode, syms))
    {
        PopulateCallArgs(grandparent, ref);
    }
}

static bool IsTypeSpecifierSymbol(TSSymbol sym, const parser::GrammarSymbols& syms) noexcept
{
    return sym == syms.symDatatype || sym == syms.symTemplateTypeList || sym == syms.symBaseClassList ||
           sym == syms.symType;
}

static bool IsTypeSpecifierContext(TSNode parent, const parser::GrammarSymbols& syms) noexcept
{
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

static bool ShouldSkipReferenceCapture(ScopeKind scopeKind, uint32_t startByte, uint32_t endByte,
                                       const LocalScopeCollector::DocumentContext& docCtx)
{
    const bool isDeclarationScope =
        scopeKind == ScopeKind::Global || scopeKind == ScopeKind::Namespace || scopeKind == ScopeKind::Class;
    if (isDeclarationScope && docCtx.hasMetadata)
    {
        return docCtx.Contains(startByte, endByte);
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

static bool HasBaseClassList(TSNode classNode)
{
    const auto& gs = parser::GetGrammarSymbols();
    const uint32_t childCount = ts_node_named_child_count(classNode);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        if (ts_node_symbol(ts_node_named_child(classNode, i)) == gs.symBaseClassList)
        {
            return true;
        }
    }
    return false;
}

static bool IsBaseConstructorCall(TSNode node, std::string_view sourceCode)
{
    if (ts_node_is_null(node))
    {
        return false;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSNode function = parser::FindAncestorWithSymbol(parser::GetParent(node), gs.symFuncDeclaration);
    if (ts_node_is_null(function) ||
        !ts_node_is_null(parser::GetChildByField(function, parser::GetGrammarSymbols().fieldReturnType)))
    {
        return false;
    }

    const TSNode functionName = parser::GetChildByField(function, parser::GetGrammarSymbols().fieldName);
    const TSNode owner = parser::FindAncestorWithSymbol(parser::GetParent(function), gs.symClassDeclaration);
    if (ts_node_is_null(functionName) || ts_node_is_null(owner))
    {
        return false;
    }

    const TSNode className = parser::GetChildByField(owner, parser::GetGrammarSymbols().fieldName);
    if (ts_node_is_null(className) ||
        parser::NodeText(className, sourceCode) != parser::NodeText(functionName, sourceCode))
    {
        return false;
    }

    return HasBaseClassList(owner);
}

void LocalScopeCollector::EnrichReferenceContext(TSNode captureNode, TSNode parent, std::string_view sourceCode,
                                                 LocalReference& ref, const DocumentContext& docCtx) const
{
    if (ref.name == "super")
    {
        ref.isSuperBaseCall = IsBaseConstructorCall(captureNode, sourceCode);
    }
    else if (ref.name == "function" && ts_node_symbol(parent) == m_gs.symLambdaExpression)
    {
        ref.isLambdaFunctionKeyword = 1;
    }

    if (!docCtx.treeHasError && !docCtx.hasMetadata)
    {
        ref.inMetadataOrError = 0;
    }
    else
    {
        ref.inMetadataOrError = docCtx.Contains(ref.startByte, ref.endByte) ? 1 : 0;
    }
}

/**
 * @brief Tests whether a field child of @p parent covers exactly the same bytes as @p expression.
 * @param[in] parent Node owning the field.
 * @param[in] field Grammar field identifier to inspect.
 * @param[in] expression Candidate expression node.
 * @return True when the field exists and spans the same byte range as @p expression.
 */
static bool IsFieldExpression(TSNode parent, TSFieldId field, TSNode expression) noexcept
{
    const TSNode fieldNode = parser::GetChildByField(parent, field);
    return !ts_node_is_null(fieldNode) && ts_node_start_byte(fieldNode) == ts_node_start_byte(expression) &&
           ts_node_end_byte(fieldNode) == ts_node_end_byte(expression);
}

/**
 * @brief Tests whether @p expression is mutated by its direct @p parent.
 * @param[in] expression Expression node under test.
 * @param[in] parent Direct parent of @p expression.
 * @param[in] gs Cached grammar symbols.
 * @return True for the left side of a plain or compound assignment and the operand of prefix/postfix `++`/`--`;
 *         false for every other operator (`-x`, `!x`, comparisons).
 */
static bool IsMutatedByParent(TSNode expression, TSNode parent, const parser::GrammarSymbols& gs) noexcept
{
    const TSSymbol symbol = ts_node_symbol(parent);
    if (symbol == gs.symAssignmentExpression)
    {
        return IsFieldExpression(parent, gs.fieldLeft, expression);
    }
    if (symbol != gs.symUnaryExpression && symbol != gs.symPostfixExpression)
    {
        return false;
    }
    const TSNode op = parser::GetChildByField(parent, gs.fieldOperator);
    const bool isStep =
        !ts_node_is_null(op) && (ts_node_symbol(op) == gs.tokPlusPlus || ts_node_symbol(op) == gs.tokDashDash);
    return isStep && IsFieldExpression(parent, gs.fieldOperand, expression);
}

/**
 * @brief Decides whether a reference identifier is the storage written by an assignment or increment.
 * @param[in] refNode Identifier node of the reference.
 * @param[in] parent Pre-captured parent of @p refNode.
 * @param[in] grandparent Pre-captured grandparent of @p refNode.
 * @param[in] gs Cached grammar symbols.
 * @return True when the identifier itself, or the member/scoped expression it terminates, is mutated; in `e.hp = 1`
 *         only `hp` qualifies.
 */
static bool IsWriteTargetNode(TSNode refNode, TSNode parent, TSNode grandparent,
                              const parser::GrammarSymbols& gs) noexcept
{
    if (ts_node_is_null(parent))
    {
        return false;
    }
    if (IsMutatedByParent(refNode, parent, gs))
    {
        return true;
    }
    if (ts_node_is_null(grandparent))
    {
        return false;
    }
    const TSSymbol symbol = ts_node_symbol(parent);
    const bool isTail = (symbol == gs.symMemberExpression && IsFieldExpression(parent, gs.fieldMember, refNode)) ||
                        (symbol == gs.symScopedIdentifier && ts_node_eq(ScopedIdentifierTail(parent), refNode));
    return isTail && IsMutatedByParent(parent, grandparent, gs);
}

void LocalScopeCollector::ProcessReferenceCapture(const RawCapture& capture, Scope* current,
                                                  const std::string& sourceCode, DocumentContext& docCtx) const
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
    if (ShouldSkipReferenceCapture(current->kind, capture.startByte, capture.endByte, docCtx) ||
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
    ref.name = std::string(parser::NodeText(capture.node, sourceCode));
    ref.startLine = startPt.row;
    ref.startCharacter = startPt.column;
    ref.endLine = endPt.row;
    ref.endCharacter = endPt.column;
    ref.startByte = capture.startByte;
    ref.endByte = capture.endByte;

    if (!ts_node_is_null(parent))
    {
        TSNode grandparent = capture.grandparent;
        if (ts_node_is_null(grandparent))
        {
            grandparent = parser::GetParent(parent);
        }
        ref.isNamedArgument = IsNamedArgumentNode(capture.node, parent);
        ref.isTypeSpecifier = IsTypeSpecifierContext(parent, m_gs);
        ref.isMemberAccess =
            IsMemberAccessNode(capture.node, parent, m_gs.symMemberExpression, m_gs.symScopedIdentifier);
        ref.isWriteTarget = IsWriteTargetNode(capture.node, parent, grandparent, m_gs) ? 1 : 0;
        DetermineCallReferenceInfo(capture.node, parent, grandparent, ref);
        if (ref.isCall)
        {
            const TSNode callNode = (ts_node_symbol(parent) == m_gs.symCallExpression) ? parent : grandparent;
            IndexCallArguments(callNode, docCtx);
        }
        IndexReferenceReceiver(parent, sourceCode, ref, docCtx);
        EnrichReferenceContext(capture.node, parent, sourceCode, ref, docCtx);
    }

    current->references.push_back(std::move(ref));
}

void LocalScopeCollector::ProcessStringCallRefCapture(const RawCapture& capture, Scope* current,
                                                      const std::string& sourceCode) const
{
    if (ts_node_has_error(capture.node) || !current)
    {
        return;
    }
    std::string_view text = parser::NodeText(capture.node, sourceCode);
    text = parser::UnquoteStringView(text);
    if (text.empty() || text.size() > 128)
    {
        return;
    }
    text = utils::CleanCandidateIdentifier(text);
    if (!utils::IsValidIdentifierOrQualified(text))
    {
        return;
    }
    TSPoint pt = ts_node_start_point(capture.node);
    if (!current->stringLiteralCallRefs)
    {
        current->stringLiteralCallRefs = std::make_unique<std::vector<StringLiteralCallRef>>();
    }
    current->stringLiteralCallRefs->push_back(
        StringLiteralCallRef{std::string(text), static_cast<uint32_t>(pt.row), static_cast<uint32_t>(pt.column)});
}

std::unique_ptr<Scope> LocalScopeCollector::BuildScopeTree(std::vector<RawCapture>& captures,
                                                           const std::string& sourceCode, DocumentContext& docCtx) const
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
            const bool hadRoot = root != nullptr;
            ProcessScopeCapture(capture, stack, root, sourceCode);
            if (!hadRoot && root)
            {
                auto context = std::make_shared<ScopeEnclosingContext>(*root->enclosingContext);
                context->receivers = docCtx.receivers;
                root->enclosingContext = std::move(context);
            }
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
            ProcessReferenceCapture(capture, stack.back().scope, sourceCode, docCtx);
        }
        else if (capture.kind == CaptureKind::StringCallRef)
        {
            ProcessStringCallRefCapture(capture, stack.back().scope, sourceCode);
        }
    }

    if (docCtx.receivers && !docCtx.receivers->callArguments.empty())
    {
        std::sort(docCtx.receivers->callArguments.begin(), docCtx.receivers->callArguments.end(),
                  [](const IndexedCallArgumentRef& a, const IndexedCallArgumentRef& b)
                  {
                      if (a.argStartByte != b.argStartByte)
                      {
                          return a.argStartByte < b.argStartByte;
                      }
                      return a.argEndByte < b.argEndByte;
                  });
        docCtx.receivers->callArguments.erase(
            std::unique(docCtx.receivers->callArguments.begin(), docCtx.receivers->callArguments.end(),
                        [](const IndexedCallArgumentRef& a, const IndexedCallArgumentRef& b)
                        {
                            return a.argStartByte == b.argStartByte && a.argEndByte == b.argEndByte &&
                                   a.calleeStartByte == b.calleeStartByte && a.argumentIndex == b.argumentIndex;
                        }),
            docCtx.receivers->callArguments.end());
    }

    return root;
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
                            std::string argText(parser::NodeText(innerType, sourceCode));
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

/**
 * @brief Reads type information and source ranges for a parameter AST node.
 * @param[in] declaratorNode Parameter AST declarator node.
 * @param[in] sourceCode Source text of the document.
 * @param[out] def LocalDefinition receiving extracted type info and default value.
 */
static std::string NormalizeParameterTypeName(std::string_view slice, bool& isReference)
{
    if (const size_t refLen = TrailingReferenceModifierLength(slice); refLen > 0)
    {
        isReference = true;
        std::string_view refMod = slice.substr(slice.size() - refLen);
        std::string_view base = slice.substr(0, slice.size() - refLen);
        TrimTypeWhitespace(base);
        if (base.empty())
        {
            return std::string(refMod);
        }
        if (refMod.ends_with("inout"))
        {
            return std::string(base) + " &inout";
        }
        if (refMod.ends_with("out"))
        {
            return std::string(base) + " &out";
        }
        if (refMod.ends_with("in"))
        {
            return std::string(base) + " &in";
        }
        return std::string(base) + " &";
    }
    return std::string(slice);
}

static std::string ExtractParamDefaultValue(TSNode declaratorNode, const std::string& sourceCode)
{
    TSTreeCursor cursor = ts_tree_cursor_new(declaratorNode);
    std::string result;
    if (ts_tree_cursor_goto_first_child(&cursor))
    {
        bool foundEq = false;
        do
        {
            TSNode child = ts_tree_cursor_current_node(&cursor);
            if (foundEq)
            {
                result = std::string(parser::NodeText(child, sourceCode));
                break;
            }
            if (parser::NodeText(child, sourceCode) == "=")
            {
                foundEq = true;
            }
        } while (ts_tree_cursor_goto_next_sibling(&cursor));
    }
    ts_tree_cursor_delete(&cursor);
    return result;
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

    TSNode nameNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldName);
    TSNode paramTypeNode = parser::GetChildByField(declaratorNode, parser::GetGrammarSymbols().fieldParamType);
    if (!ts_node_is_null(paramTypeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(paramTypeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.isConst = typeInfo.isConst;
        def.isReference = typeInfo.isReference;
        def.typeKind = typeInfo.kind;
        const uint32_t tStart = ts_node_start_byte(paramTypeNode);
        const uint32_t tEnd = (!ts_node_is_null(nameNode) && ts_node_start_byte(nameNode) > tStart)
                                  ? ts_node_start_byte(nameNode)
                                  : ts_node_end_byte(paramTypeNode);
        std::string_view slice = utils::TrimView(std::string_view(sourceCode).substr(tStart, tEnd - tStart));
        bool isRef = def.isReference;
        def.typeName = NormalizeParameterTypeName(slice, isRef);
        def.isReference = isRef;
        PopulateTypeRanges(paramTypeNode, sourceCode, def);
    }

    def.defaultValue = ExtractParamDefaultValue(declaratorNode, sourceCode);
}

void LocalScopeCollector::PopulateVariableFromField(TSNode parentNode, TSSymbol fieldSymbol,
                                                    const std::string& sourceCode, LocalDefinition& def) const
{
    TSPoint rStart = ts_node_start_point(parentNode);
    TSPoint rEnd = ts_node_end_point(parentNode);
    def.fullStartLine = rStart.row;
    def.fullStartCharacter = rStart.column;
    def.fullEndLine = rEnd.row;
    def.fullEndCharacter = rEnd.column;

    TSNode typeNode = parser::GetChildByField(parentNode, fieldSymbol);
    if (!ts_node_is_null(typeNode))
    {
        TypeExtractionResult typeInfo = ExtractTypeInfoFromAST(typeNode, sourceCode);
        def.isHandleType = typeInfo.isHandle;
        def.isConst = typeInfo.isConst;
        def.isReference = typeInfo.isReference;
        def.typeKind = typeInfo.kind;
        def.typeName = std::string(parser::NodeText(typeNode, sourceCode));
        PopulateTypeRanges(typeNode, sourceCode, def);
    }
}

/**
 * @brief Populates type metadata and full source range for a foreach loop variable declaration.
 * @param[in] declaratorNode AST declarator node of the foreach variable.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] def LocalDefinition receiving type extraction and location bounds.
 */
void LocalScopeCollector::ReadForeachVariableTypeInfo(TSNode declaratorNode, const std::string& sourceCode,
                                                      LocalDefinition& def) const
{
    PopulateVariableFromField(declaratorNode, parser::GetGrammarSymbols().fieldType, sourceCode, def);
}

/**
 * @brief Reads declared type information and bounds for lambda parameters.
 * @param[in] nameNode AST node of the lambda parameter identifier.
 * @param[in] declaratorNode AST node of the enclosing lambda_parameter_list.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[out] def LocalDefinition receiving extracted type info.
 */
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
        def.isConst = typeInfo.isConst;
        def.isReference = typeInfo.isReference;
        def.typeKind = typeInfo.kind;
        def.typeName = std::string(parser::NodeText(typeNode, sourceCode));
        PopulateTypeRanges(typeNode, sourceCode, def);
    }
}

/**
 * @brief Counts the number of named variable declarator child nodes matching a symbol.
 * @param[in] declarationNode AST declaration node to inspect.
 * @param[in] declaratorSym Grammar symbol ID of the declarator node.
 * @return Total number of matching declarator child nodes found.
 */
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

/**
 * @brief Populates type metadata and full source location for a variable declarator within a declaration statement.
 * @param[in] declaratorNode Specific AST variable declarator node being evaluated.
 * @param[in] declarationNode Parent AST declaration node containing shared type information.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] def LocalDefinition receiving type extraction and location bounds.
 */
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
        def.isConst = typeInfo.isConst;
        def.isReference = typeInfo.isReference;
        def.typeKind = typeInfo.kind;
        def.typeName = std::string(parser::NodeText(typeNode, sourceCode));
        PopulateTypeRanges(typeNode, sourceCode, def);
    }

    TSNode valueNode = parser::FindDeclaratorValueNode(declaratorNode, sourceCode);
    if (!ts_node_is_null(valueNode))
    {
        def.defaultValue = std::string(parser::NodeText(valueNode, sourceCode));
    }
    def.hasNullInitializer = IsNullInitializer(valueNode);
}

/**
 * @brief Reads variable type info from an empty-parentheses func declaration.
 * @param funcNode Function AST node.
 * @param sourceCode Source text.
 * @param def Local definition to populate.
 */
void LocalScopeCollector::ReadEmptyParenVariableTypeInfo(TSNode funcNode, const std::string& sourceCode,
                                                         LocalDefinition& def) const
{
    PopulateVariableFromField(funcNode, parser::GetGrammarSymbols().fieldReturnType, sourceCode, def);
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

    if (ts_node_symbol(declaratorNode) == m_gs.symFuncDeclaration)
    {
        ReadEmptyParenVariableTypeInfo(declaratorNode, sourceCode, def);
        return;
    }
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

/**
 * @brief Local scope collector read virtual property type info.
 * @param[in] propertyNode AST node under evaluation.
 * @param[in] sourceCode Document source text.
 * @param[in,out] def LocalDefinition being populated with variable metadata.
 */
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
        def.isConst = typeInfo.isConst;
        def.isReference = typeInfo.isReference;
        def.typeKind = typeInfo.kind;
        def.typeName = std::string(parser::NodeText(typeNode, sourceCode));
        PopulateTypeRanges(typeNode, sourceCode, def);
    }
}

static Scope* FindInnermostScopeMut(Scope* root, uint32_t line, uint32_t character)
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

static void InsertRecoveredDefinition(Scope* targetScope, LocalDefinition&& def)
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

/**
 * @brief Extracts the target identifier node, name, and handle flag from an assignment left-hand expression.
 * @param[in] leftNode Left-hand AST expression node of an assignment.
 * @param[in] gs Precompiled grammar symbol mappings.
 * @param[in] sourceCode Source text of the document.
 * @param[out] outName Extracted variable name string.
 * @param[out] outIsHandle True if declared as a handle type.
 * @return Identifier AST node if extraction succeeded; null node otherwise.
 */
static TSNode ExtractAssignmentLeftVar(TSNode leftNode, const parser::GrammarSymbols& gs, std::string_view sourceCode,
                                       std::string& outName, bool& outIsHandle)
{
    const TSSymbol sym = ts_node_symbol(leftNode);
    if (sym == gs.symIdentifier)
    {
        outName = std::string(parser::NodeText(leftNode, sourceCode));
        outIsHandle = false;
        return !outName.empty() ? leftNode : TSNode{};
    }
    if (sym == gs.symScopedIdentifier)
    {
        TSNode id = parser::GetChildByField(leftNode, parser::GetGrammarSymbols().fieldName);
        if (ts_node_is_null(id))
        {
            const uint32_t count = ts_node_child_count(leftNode);
            id = (count > 0) ? ts_node_child(leftNode, count - 1) : TSNode{};
        }
        outName = std::string(parser::NodeText(id, sourceCode));
        outIsHandle = false;
        return !outName.empty() ? id : TSNode{};
    }
    if (sym == gs.symUnaryExpression)
    {
        std::string_view text = parser::NodeText(leftNode, sourceCode);
        if (text.starts_with('@'))
        {
            TSNode operand = parser::GetChildByField(leftNode, parser::GetGrammarSymbols().fieldOperand);
            if (ts_node_is_null(operand))
            {
                operand = ts_node_child(leftNode, 1);
            }
            TSNode res = ExtractAssignmentLeftVar(operand, gs, sourceCode, outName, outIsHandle);
            if (!ts_node_is_null(res))
            {
                outIsHandle = true;
                return res;
            }
        }
    }
    return TSNode{};
}

/**
 * @brief Registers a recovered variable declaration into the innermost lexical scope.
 * @param[in,out] rootScope Lexical scope tree root.
 * @param[in] identNode AST node of the identifier.
 * @param[in] varName Name of the recovered variable.
 * @param[in] typeName Declared type name to attach to the recovered local definition.
 * @param[in] isHandle True if the variable is declared as a handle.
 */
static void RegisterRecoveredVariable(Scope* rootScope, TSNode identNode, std::string varName, std::string typeName,
                                      bool isHandle)
{
    if (isHandle && !IsHandleTypeView(typeName))
    {
        typeName += "@";
    }
    const TSPoint pt = ts_node_start_point(identNode);
    const TSPoint endPt = ts_node_end_point(identNode);
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

/**
 * @brief Recovers split variable declarations across if-statement consequence expressions.
 * @param[in] node If-statement AST node under evaluation.
 * @param[in,out] rootScope Lexical scope tree root for registering recovered definitions.
 * @param[in] sourceCode Source text of the document.
 * @param[in] gs Precompiled grammar symbol mappings.
 * @return True if a split variable declaration was successfully recovered; false otherwise.
 */
static bool TryRecoverSplitIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
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
    std::string_view typeView = parser::NodeText(ts_node_child(cons, 0), sourceCode);
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
    bool isHandleVar = false;
    TSNode identNode = ExtractAssignmentLeftVar(left, gs, sourceCode, varName, isHandleVar);
    if (!ts_node_is_null(identNode))
    {
        RegisterRecoveredVariable(rootScope, identNode, std::move(varName), std::string(typeView), isHandleVar);
        return true;
    }
    return false;
}

/**
 * @brief Scans AST error nodes inside if-statement headers to extract and register recovered variable types.
 * @param[in] node If-statement AST node containing parse errors.
 * @param[in,out] rootScope Lexical scope tree root for registering recovered definitions.
 * @param[in] sourceCode Source text of the document.
 * @param[in] gs Precompiled grammar symbol mappings.
 */
static void TryRecoverErrorIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
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
            errTypeView = parser::NodeText(child, sourceCode);
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
    bool isHandleVar = false;
    TSNode identNode = ExtractAssignmentLeftVar(left, gs, sourceCode, varName, isHandleVar);
    if (!ts_node_is_null(identNode))
    {
        RegisterRecoveredVariable(rootScope, identNode, std::move(varName), std::move(errTypeName), isHandleVar);
    }
}

static void TryRecoverUnbracedIfDeclaration(TSNode node, Scope* rootScope, const std::string& sourceCode,
                                            const parser::GrammarSymbols& gs)
{
    if (TryRecoverSplitIfDeclaration(node, rootScope, sourceCode, gs))
    {
        return;
    }
    TryRecoverErrorIfDeclaration(node, rootScope, sourceCode, gs);
}

static void ProcessVirtualPropertySetAccessor(TSNode accNode, const VirtualPropertyContext& ctx)
{
    TSNode kindNode = parser::GetChildByField(accNode, ctx.gs.fieldKind);
    if (ts_node_is_null(kindNode))
    {
        return;
    }
    const bool isSet =
        (ts_node_symbol(kindNode) == ctx.gs.tokSet || parser::NodeText(kindNode, ctx.sourceCode) == "set");
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

static void ProcessVirtualPropertyNode(TSNode propNode, Scope* root, const parser::GrammarSymbols& gs,
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
    std::string propTypeStr(parser::NodeText(propTypeNode, sourceCode));
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

void LocalScopeCollector::RegisterVirtualPropertySetParameters(const NodeIndex& nodeIndex, Scope* rootScope,
                                                               const std::string& sourceCode) const
{
    for (TSNode property : nodeIndex.Nodes(m_gs.symVirtualProperty))
    {
        ProcessVirtualPropertyNode(property, rootScope, m_gs, sourceCode);
    }
}
} // namespace angel_lsp::analysis
