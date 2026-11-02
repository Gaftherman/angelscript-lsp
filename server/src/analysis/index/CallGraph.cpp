#include "analysis/index/CallGraph.h"
#include "analysis/index/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <string>
#include <utility>

namespace angel_lsp::analysis
{
/**
 * @brief Computes the next container qualified name index for an AST container node.
 * @param[in] node AST node representing the enclosing container.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[in] current Current container index in the naming context.
 * @return Updated container index in the naming context.
 */
uint32_t NamingContext::NextContainer(TSNode node, std::string_view sourceCode, uint32_t current)
{
    const std::string name =
        parser::NodeText(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName), sourceCode);
    if (name.empty())
    {
        return current;
    }
    const std::string& parent = containers[current];
    containers.push_back(utils::JoinScopedName(parent, name));
    return static_cast<uint32_t>(containers.size() - 1);
}

/**
 * @brief Computes the next caller qualified name index for an AST function node.
 * @param[in] node AST node representing the function declaration.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[in] containerIdx Index of the enclosing container.
 * @param[in] current Current caller index in the naming context.
 * @return Updated caller index in the naming context.
 */
uint32_t NamingContext::NextCaller(TSNode node, std::string_view sourceCode, uint32_t containerIdx, uint32_t current)
{
    const std::string name =
        parser::NodeText(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName), sourceCode);
    if (name.empty())
    {
        return current;
    }
    const std::string& parent = containers[containerIdx];
    callers.push_back(utils::JoinScopedName(parent, name));
    return static_cast<uint32_t>(callers.size() - 1);
}

/**
 * @brief Extracts the AST node identifying the callee function in a call expression.
 * @param[in] callNode AST node representing the call expression.
 * @return AST node of the callee name identifier, or null node if none stable.
 */
static TSNode CalleeNameNode(TSNode callNode)
{
    TSNode callee = parser::GetChildByField(callNode, parser::GetGrammarSymbols().fieldFunction);
    if (ts_node_is_null(callee))
    {
        return TSNode{};
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol calleeSym = ts_node_symbol(callee);

    if (calleeSym == gs.symMemberExpression)
    {
        return parser::GetChildByField(callee, gs.fieldMember);
    }

    if (calleeSym == gs.symScopedIdentifier)
    {
        TSNode last{};
        const uint32_t count = ts_node_named_child_count(callee);
        for (uint32_t i = 0; i < count; ++i)
        {
            TSNode child = ts_node_named_child(callee, i);
            if (ts_node_symbol(child) == gs.symIdentifier)
            {
                last = child;
            }
        }
        return last;
    }

    if (calleeSym == gs.symIdentifier)
    {
        return callee;
    }

    return TSNode{};
}

/**
 * @brief Checks if a tree-sitter grammar symbol represents a container declaration.
 * @param[in] sym Tree-sitter grammar symbol.
 * @return True if the symbol is a class, interface, mixin, or namespace declaration; false otherwise.
 */
static bool IsContainerSymbol(TSSymbol sym)
{
    const auto& gs = parser::GetGrammarSymbols();
    return sym == gs.symClassDeclaration || sym == gs.symInterfaceDeclaration || sym == gs.symMixinDeclaration ||
           sym == gs.symNamespaceDeclaration;
}

/**
 * @brief Records a resolved call site into the destination vector.
 * @param[in] node AST call expression node.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[in] caller Qualified name of the enclosing caller.
 * @param[in,out] out Vector receiving the appended call site.
 */
static void RecordCallSite(TSNode node, std::string_view sourceCode, const std::string& caller,
                           std::vector<CallSite>& out)
{
    const TSNode nameNode = CalleeNameNode(node);
    if (ts_node_is_null(nameNode))
    {
        return;
    }
    std::string callee = parser::NodeText(nameNode, sourceCode);
    if (!callee.empty())
    {
        out.push_back(CallSite{caller, std::move(callee), ToSourceRange(nameNode)});
    }
}

/**
 * @brief Traverses the AST with a flat worklist, collecting call expressions into a vector.
 * @param[in] root AST root node.
 * @param[in] sourceCode Source text buffer of the document.
 * @param[in,out] out Vector receiving collected call sites.
 */
static void CollectCallsInto(TSNode root, std::string_view sourceCode, std::vector<CallSite>& out)
{
    NamingContext ctx;
    std::vector<TraversalItem> worklist;
    worklist.push_back(TraversalItem{root, 0, 0});

    while (!worklist.empty())
    {
        const TraversalItem item = worklist.back();
        worklist.pop_back();

        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(item.node);
        uint32_t nextContainer = item.containerIndex;
        uint32_t nextCaller = item.callerIndex;

        if (IsContainerSymbol(sym))
        {
            nextContainer = ctx.NextContainer(item.node, sourceCode, item.containerIndex);
        }
        else if (sym == gs.symFuncDeclaration)
        {
            nextCaller = ctx.NextCaller(item.node, sourceCode, item.containerIndex, item.callerIndex);
        }
        else if (sym == gs.symCallExpression)
        {
            RecordCallSite(item.node, sourceCode, ctx.callers[item.callerIndex], out);
        }

        const uint32_t childCount = ts_node_named_child_count(item.node);
        for (uint32_t i = childCount; i > 0; --i)
        {
            worklist.push_back(TraversalItem{ts_node_named_child(item.node, i - 1), nextContainer, nextCaller});
        }
    }
}

/**
 * @brief Collects all function call expressions found in an AST document.
 * @param[in] root Root node of the document's AST.
 * @param[in] sourceCode Source text buffer of the document.
 * @return Vector of collected CallSite structures in document order.
 */
std::vector<CallSite> CollectCalls(TSNode root, std::string_view sourceCode)
{
    std::vector<CallSite> calls;
    if (ts_node_is_null(root) || sourceCode.empty())
    {
        return calls;
    }

    CollectCallsInto(root, sourceCode, calls);
    return calls;
}

/**
 * @brief Filters call sites across documents matching a specific string field against a target value.
 * @param[in] byDocument Map of file URIs to document call sites.
 * @param[in] mutex Shared mutex guarding concurrent access to the map.
 * @param[in] field Pointer-to-member selecting the CallSite string field to compare.
 * @param[in] target Target identifier string to match.
 * @return Vector of DocumentCalls matching the specified filter criteria.
 */
static std::vector<DocumentCalls>
FilterCallsByField(const ankerl::unordered_dense::map<std::string, std::vector<CallSite>>& byDocument,
                   std::shared_mutex& mutex, std::string CallSite::* field, const std::string& target)
{
    std::vector<DocumentCalls> found;
    if (target.empty())
    {
        return found;
    }

    std::shared_lock lock(mutex);
    for (const auto& [fileUri, calls] : byDocument)
    {
        DocumentCalls matches{fileUri, {}};
        for (const auto& call : calls)
        {
            if (call.*field == target)
            {
                matches.calls.push_back(call);
            }
        }
        if (!matches.calls.empty())
        {
            found.push_back(std::move(matches));
        }
    }
    return found;
}

/**
 * @brief Replaces all calls recorded for the specified document URI.
 * @param[in] fileUri URI of the document being updated.
 * @param[in] calls Vector of call sites identified in the document.
 */
void CallGraphIndex::SetDocumentCalls(const std::string& fileUri, std::vector<CallSite> calls)
{
    std::unique_lock lock(m_mutex);
    if (calls.empty())
    {
        m_byDocument.erase(fileUri);
        return;
    }
    m_byDocument[fileUri] = std::move(calls);
}

/**
 * @brief Discards all calls recorded for the specified document URI.
 * @param[in] fileUri URI of the document to clear.
 */
void CallGraphIndex::ClearDocument(const std::string& fileUri)
{
    std::unique_lock lock(m_mutex);
    m_byDocument.erase(fileUri);
}

/**
 * @brief Finds every call to a given bare name across every indexed document.
 * @param[in] calleeName Bare name of the callee function to look for.
 * @return Vector of DocumentCalls matching the callee name.
 */
std::vector<DocumentCalls> CallGraphIndex::FindCallsTo(const std::string& calleeName) const
{
    return FilterCallsByField(m_byDocument, m_mutex, &CallSite::callee, calleeName);
}

/**
 * @brief Finds every call written inside a function identified by its qualified name.
 * @param[in] callerQualifiedName Fully qualified name of the calling function.
 * @return Vector of DocumentCalls originating from the caller.
 */
std::vector<DocumentCalls> CallGraphIndex::FindCallsFrom(const std::string& callerQualifiedName) const
{
    return FilterCallsByField(m_byDocument, m_mutex, &CallSite::caller, callerQualifiedName);
}
} // namespace angel_lsp::analysis
