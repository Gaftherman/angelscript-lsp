#include "analysis/checkers/NamespaceChecker.h"

#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <cctype>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Whether a node sits inside a `mixin class` body.
 *        A mixin is compiled into the class that includes it, so an unqualified name in its body
 */
static bool IsInsideMixinBody(TSNode node)
{
    const auto& gs = parser::GetGrammarSymbols();
    TSNode mixinNode = parser::FindAncestorWithSymbol(parser::GetParent(node), gs.symMixinDeclaration);
    return !ts_node_is_null(mixinNode);
}
/**
 * @brief Checks if a scoped identifier refers to a known namespace or type prefix.
 */
static void CheckScopedIdentifier(TSNode node, std::string_view sourceCode, const SymbolTable& table,
                                  DiagnosticContext& ctx)
{
    const uint32_t childCount = ts_node_child_count(node);
    if (childCount < 3)
    {
        return;
    }
    TSNode firstChild = ts_node_named_child(node, 0);
    if (ts_node_is_null(firstChild) || ts_node_symbol(firstChild) != parser::GetGrammarSymbols().symIdentifier)
    {
        return;
    }
    std::string prefix = parser::GetNodeText(firstChild, sourceCode);
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.front())))
    {
        prefix.erase(prefix.begin());
    }
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.back())))
    {
        prefix.pop_back();
    }
    const auto& usings = GetUsingNamespaces(ctx.request);
    if (!prefix.empty() && !IsKnownScope(prefix, node, sourceCode, table, usings, ctx.request.nodeIndex))
    {
        TSPoint startPt = ts_node_start_point(firstChild);
        TSPoint endPt = ts_node_end_point(firstChild);
        ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-undefined-namespace", prefix,
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Extracts and trims the callee name from a call expression node.
 */
static std::string_view ExtractCalleeName(TSNode node, std::string_view sourceCode)
{
    TSNode funcNode = parser::GetCallCallee(node);
    if (ts_node_is_null(funcNode))
    {
        return {};
    }
    return parser::Trim(parser::GetNodeTextView(funcNode, sourceCode));
}

static bool CheckNamespaceOrClassMembers(std::string_view name, const rules::ContainerMembers& members, bool reqKeyword)
{
    const auto& accSet = reqKeyword ? members.keywordAccessorPropertyNames : members.accessorPropertyNames;
    return accSet.contains(name) || members.allMemberNames.contains(name);
}

static bool IsEnclosingContainerCallee(std::string_view name, TSNode node, const NamespaceCheckRequest& request,
                                       const DiagnosticContext& ctx)
{
    if (!request.nodeIndex)
    {
        return false;
    }
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const uint32_t byteOffset = ts_node_start_byte(node);
    const bool reqKeyword = ctx.request.RequiresAccessorKeyword();
    const auto containers = request.nodeIndex->CollectEnclosingContainers(byteOffset, request.sourceCode);
    for (const auto& container : containers)
    {
        if (container.kind == ContainerKind::Namespace)
        {
            const std::string nsName = container.qualifiedName.empty() ? container.name : container.qualifiedName;
            if (CheckNamespaceOrClassMembers(name, ruleIndex.NamespaceMembers(nsName), reqKeyword))
            {
                return true;
            }
            continue;
        }
        if (container.kind != ContainerKind::Class && container.kind != ContainerKind::Interface)
        {
            continue;
        }
        const auto hierarchy = GetInheritedTypeHierarchy(
            container.qualifiedName.empty() ? container.name : container.qualifiedName, ctx.request.symbolTable);
        for (const auto& typeName : hierarchy)
        {
            if (CheckNamespaceOrClassMembers(name, ruleIndex.ClassMembers(typeName), reqKeyword))
            {
                return true;
            }
        }
    }
    return false;
}

static bool IsCalleeAccessorProperty(std::string_view name, TSNode node, const NamespaceCheckRequest& request,
                                     const DiagnosticContext& ctx)
{
    const auto& ruleIndex = ctx.request.GetRuleIndex();
    const auto& globalAccessors = ctx.request.RequiresAccessorKeyword() ? ruleIndex.keywordGlobalAccessorPropertyNames
                                                                        : ruleIndex.globalAccessorPropertyNames;
    if (globalAccessors.contains(name) || IsEnclosingContainerCallee(name, node, request, ctx))
    {
        return true;
    }

    return !FindGlobalPropertyAccessors(std::string(name), ctx.request.symbolTable,
                                        ctx.request.RequiresAccessorKeyword())
                .empty();
}

static bool IsSpecialContextCallee(std::string_view name, TSNode node, const NamespaceCheckRequest& request)
{
    if (IsReservedKeyword(std::string(name)))
    {
        return true;
    }

    if (request.nodeIndex && request.nodeIndex->IsInsideError(ts_node_start_byte(node)))
    {
        return true;
    }

    if (name == "super")
    {
        TSNode funcNode = parser::GetCallCallee(node);
        return IsBaseConstructorCall(funcNode, request.sourceCode);
    }

    return false;
}

static bool IsKnownCalleeSymbol(std::string_view name, std::string_view getterName, const DiagnosticContext& ctx)
{
    return ctx.request.IsRegisteredSymbol(name) || ctx.request.symbolTable.HasSymbol(name) ||
           ctx.request.IsRegisteredSymbol(getterName) || ctx.request.symbolTable.HasSymbol(getterName);
}

/**
 * @brief Checks if a callee name is known in scope, registered, or excused by context.
 */
static bool IsKnownCallee(std::string_view name, TSNode node, const NamespaceCheckRequest& request,
                          const DiagnosticContext& ctx)
{
    if (IsSpecialContextCallee(name, node, request))
    {
        return true;
    }

    const Scope* scope = ctx.request.scopeRoot
                             ? FindEnclosingScope(ctx.request.scopeRoot.get(), ts_node_start_point(node).row,
                                                  ts_node_start_point(node).column)
                             : nullptr;
    if (scope && ResolveInScope(scope, name))
    {
        return true;
    }

    if (!FindSymbolsInScope(std::string(name), node, request.sourceCode, ctx.request.symbolTable, request.nodeIndex)
             .empty())
    {
        return true;
    }

    std::string getterName = "get_" + std::string(name);
    if (!FindSymbolsInScope(getterName, node, request.sourceCode, ctx.request.symbolTable, request.nodeIndex).empty())
    {
        return true;
    }

    if (IsKnownCalleeSymbol(name, getterName, ctx))
    {
        return true;
    }

    if (IsCalleeAccessorProperty(name, node, request, ctx))
    {
        return true;
    }

    return IsInsideMixinBody(node);
}

static bool IsKnownDirectGlobalCallee(std::string_view identView, const DiagnosticContext& ctx)
{
    if (ctx.request.IsRegisteredSymbol(identView))
    {
        return true;
    }
    if (auto symsPtr = ctx.request.symbolTable.FindSymbolsPtr(identView))
    {
        for (const auto& sym : *symsPtr)
        {
            if (sym.containerName.empty() && sym.type != SymbolType::CallReference)
            {
                return true;
            }
        }
    }
    if (ctx.request.scopeRoot)
    {
        if (ResolveInScope(ctx.request.scopeRoot.get(), identView) != nullptr)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if an unqualified function call resolves to an in-scope symbol.
 */
static void CheckCallExpression(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    std::string_view calleeName = ExtractCalleeName(node, request.sourceCode);
    if (calleeName.empty() || calleeName.find('.') != std::string_view::npos)
    {
        return;
    }

    if (utils::HasLeadingGlobalPrefix(calleeName))
    {
        const std::string_view identView = utils::StripGlobalPrefix(calleeName);
        if (!utils::ContainsScopeSeparator(identView))
        {
            if (!identView.empty() && !IsKnownDirectGlobalCallee(identView, ctx))
            {
                TSNode funcNode = parser::GetCallCallee(node);
                const TSPoint startPt = ts_node_start_point(funcNode);
                const TSPoint endPt = ts_node_end_point(funcNode);
                ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-undefined-identifier",
                                calleeName, DiagnosticSeverity::Error);
            }
            return;
        }
        calleeName = utils::StripGlobalPrefix(calleeName);
    }

    if (utils::ContainsScopeSeparator(calleeName))
    {
        return;
    }

    if (IsKnownCallee(calleeName, node, request, ctx))
    {
        return;
    }

    TSNode funcNode = parser::GetCallCallee(node);
    const TSPoint startPt = ts_node_start_point(funcNode);
    const TSPoint endPt = ts_node_end_point(funcNode);
    ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-undefined-identifier", calleeName,
                    DiagnosticSeverity::Error);
}

/**
 * @brief Checks if an import declaration mistakenly specifies a body block.
 */
static void CheckImportBody(TSNode node, std::string_view sourceCode, DiagnosticContext& ctx)
{
    std::string nodeText = parser::GetNodeText(node, sourceCode);
    size_t importPos = nodeText.find("import ");
    if (importPos == std::string::npos)
    {
        return;
    }
    size_t fromPos = nodeText.find("from", importPos);
    if (fromPos != std::string::npos && nodeText.find('{', fromPos) != std::string::npos)
    {
        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);
        ctx.EmitAtRange({startPt.row, startPt.column, endPt.row, endPt.column}, "as-err-import-has-body", "import",
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Dispatches node inspection to category-specific checks.
 */
static void ProcessNamespaceNode(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symScopedIdentifier)
    {
        CheckScopedIdentifier(node, request.sourceCode, ctx.request.symbolTable, ctx);
    }
    else if (sym == gs.symCallExpression)
    {
        CheckCallExpression(node, request, ctx);
    }
    else if (sym == gs.symImportDeclaration || ts_node_is_error(node))
    {
        CheckImportBody(node, request.sourceCode, ctx);
    }
}

static void CheckFromNodeIndex(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    request.nodeIndex->ForEachNodeOrdered(
        std::array{gs.symScopedIdentifier, gs.symCallExpression, gs.symImportDeclaration, gs.symError},
        [&](TSNode node) { ProcessNamespaceNode(node, request, ctx); });
}

/**
 * @brief Check from ast traversal.
 * @param[in] root Root AST node of the document or tree.
 * @param[in] request Contextual request bundle containing AST root, source text, and parameters.
 * @param[in,out] ctx Diagnostic collection and analysis context.
 */
static void CheckFromASTTraversal(TSNode root, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    if (ts_node_is_null(root))
    {
        return;
    }
    TSTreeCursor cursor = ts_tree_cursor_new(root);
    bool visitedChildren = false;
    while (true)
    {
        TSNode node = ts_tree_cursor_current_node(&cursor);
        if (!visitedChildren)
        {
            ProcessNamespaceNode(node, request, ctx);
            if (ts_tree_cursor_goto_first_child(&cursor))
            {
                continue;
            }
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor))
        {
            visitedChildren = false;
            continue;
        }
        if (ts_tree_cursor_goto_parent(&cursor))
        {
            visitedChildren = true;
            continue;
        }
        break;
    }
    ts_tree_cursor_delete(&cursor);
}

void CheckNamespacesAndScopes(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    if (request.nodeIndex)
    {
        CheckFromNodeIndex(request, ctx);
    }
    else
    {
        CheckFromASTTraversal(request.root, request, ctx);
    }
}
} // namespace angel_lsp::analysis
