#include "analysis/NamespaceChecker.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"

#include "parser/GrammarNames.h"
#include <cctype>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
/**
 * @brief Whether a node sits inside a `mixin class` body.
 *
 * A mixin is compiled into the class that includes it, so an unqualified name in its body
 * may name a member of a class this file has never seen. See the note at the caller.
 */
bool IsInsideMixinBody(TSNode node)
{
    for (TSNode current = node; !ts_node_is_null(current); current = ts_node_parent(current))
    {
        if (std::string_view(ts_node_type(current)) == "mixin_declaration")
        {
            return true;
        }
    }
    return false;
}
/**
 * @brief Checks if a scoped identifier refers to a known namespace or type prefix.
 */
void CheckScopedIdentifier(TSNode node, std::string_view sourceCode, const SymbolTable& table, DiagnosticContext& ctx)
{
    const uint32_t childCount = ts_node_child_count(node);
    if (childCount < 3)
    {
        return;
    }
    TSNode firstChild = ts_node_child(node, 0);
    if (std::string_view(ts_node_type(firstChild)) != "identifier")
    {
        return;
    }
    std::string prefix = GetNodeText(firstChild, sourceCode);
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.front())))
    {
        prefix.erase(prefix.begin());
    }
    while (!prefix.empty() && isspace(static_cast<unsigned char>(prefix.back())))
    {
        prefix.pop_back();
    }
    if (!prefix.empty() && !IsKnownScope(prefix, node, sourceCode, table))
    {
        TSPoint startPt = ts_node_start_point(firstChild);
        TSPoint endPt = ts_node_end_point(firstChild);
        ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column, "as-err-undefined-namespace", prefix,
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Extracts and trims the callee name from a call expression node.
 */
std::string ExtractCalleeName(TSNode node, std::string_view sourceCode)
{
    TSNode funcNode = parser::GetChildByField(node, parser::fields::Function);
    if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
    {
        funcNode = ts_node_child(node, 0);
    }
    if (ts_node_is_null(funcNode))
    {
        return {};
    }
    std::string name = GetNodeText(funcNode, sourceCode);
    while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
    {
        name.erase(name.begin());
    }
    while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
    {
        name.pop_back();
    }
    return name;
}

/**
 * @brief Checks if a callee name is known in scope, registered, or excused by context.
 */
bool IsKnownCallee(const std::string& name, TSNode node, const NamespaceCheckRequest& request,
                   const DiagnosticContext& ctx)
{
    if (name == "super")
    {
        TSNode funcNode = parser::GetChildByField(node, parser::fields::Function);
        if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
        {
            funcNode = ts_node_child(node, 0);
        }
        if (IsBaseConstructorCall(funcNode, request.sourceCode))
        {
            return true;
        }
    }

    const Scope* scope = ctx.request.scopeRoot
                             ? FindEnclosingScope(ctx.request.scopeRoot.get(), ts_node_start_point(node).row,
                                                  ts_node_start_point(node).column)
                             : nullptr;
    if (scope && ResolveInScope(scope, name))
    {
        return true;
    }

    if (!FindSymbolsInScope(name, node, request.sourceCode, ctx.request.symbolTable).empty())
    {
        return true;
    }

    if (ctx.request.IsRegisteredSymbol(name) || ctx.request.symbolTable.HasSymbol(name))
    {
        return true;
    }

    return IsInsideMixinBody(node);
}

/**
 * @brief Checks if an unqualified function call resolves to an in-scope symbol.
 */
void CheckCallExpression(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const std::string calleeName = ExtractCalleeName(node, request.sourceCode);
    if (calleeName.empty() || calleeName.find("::") != std::string::npos || calleeName.find('.') != std::string::npos)
    {
        return;
    }

    if (IsKnownCallee(calleeName, node, request, ctx))
    {
        return;
    }

    TSNode funcNode = parser::GetChildByField(node, parser::fields::Function);
    if (ts_node_is_null(funcNode) && ts_node_child_count(node) > 0)
    {
        funcNode = ts_node_child(node, 0);
    }
    const TSPoint startPt = ts_node_start_point(funcNode);
    const TSPoint endPt = ts_node_end_point(funcNode);
    ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column, "as-err-undefined-identifier", calleeName,
                    DiagnosticSeverity::Error);
}

/**
 * @brief Checks if an import declaration mistakenly specifies a body block.
 */
void CheckImportBody(TSNode node, std::string_view sourceCode, DiagnosticContext& ctx)
{
    std::string nodeText = GetNodeText(node, sourceCode);
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
        ctx.EmitAtRange(startPt.row, startPt.column, endPt.row, endPt.column, "as-err-import-has-body", "import",
                        DiagnosticSeverity::Error);
    }
}

/**
 * @brief Dispatches node inspection to category-specific checks.
 */
void ProcessNamespaceNode(TSNode node, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    const std::string_view type = ts_node_type(node);
    if (type == "scoped_identifier")
    {
        CheckScopedIdentifier(node, request.sourceCode, ctx.request.symbolTable, ctx);
    }
    else if (type == "call_expression")
    {
        CheckCallExpression(node, request, ctx);
    }
    else if (type == "import_declaration" || type == "ERROR")
    {
        CheckImportBody(node, request.sourceCode, ctx);
    }
}

void CheckFromNodeIndex(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    struct Cursor
    {
        std::span<const TSNode> nodes;
        size_t index = 0;
        uint32_t currentByte() const
        {
            return (index < nodes.size()) ? ts_node_start_byte(nodes[index]) : UINT32_MAX;
        }
    };

    std::array<Cursor, 4> cursors = {{{request.nodeIndex->Nodes(parser::nodes::ScopedIdentifier), 0},
                                      {request.nodeIndex->Nodes(parser::nodes::CallExpression), 0},
                                      {request.nodeIndex->Nodes(parser::nodes::ImportDeclaration), 0},
                                      {request.nodeIndex->Nodes("ERROR"), 0}}};

    while (true)
    {
        size_t best = 0;
        uint32_t minByte = cursors[0].currentByte();
        for (size_t c = 1; c < cursors.size(); ++c)
        {
            uint32_t b = cursors[c].currentByte();
            if (b < minByte)
            {
                minByte = b;
                best = c;
            }
        }
        if (minByte == UINT32_MAX)
        {
            break;
        }

        TSNode node = cursors[best].nodes[cursors[best].index++];
        ProcessNamespaceNode(node, request, ctx);
    }
}

void CheckFromASTTraversal(TSNode root, const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    std::vector<TSNode> stack = {root};
    while (!stack.empty())
    {
        TSNode node = stack.back();
        stack.pop_back();

        ProcessNamespaceNode(node, request, ctx);

        const uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            stack.push_back(ts_node_child(node, i));
        }
    }
}
} // namespace

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
