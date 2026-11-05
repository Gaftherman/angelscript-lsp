#include "analysis/index/CallGraph.h"

#include "analysis/index/SymbolTable.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "utils/ScopedNameUtils.h"

#include <algorithm>
#include <string>
#include <utility>

namespace angel_lsp::analysis
{
static uint32_t AppendScopedName(std::string_view name, std::string_view parent, std::vector<std::string>& list,
                                 uint32_t fallback)
{
    if (name.empty())
    {
        return fallback;
    }
    list.push_back(utils::JoinScopedName(parent, name));
    return static_cast<uint32_t>(list.size() - 1);
}

uint32_t NamingContext::NextContainer(TSNode node, std::string_view sourceCode, uint32_t current)
{
    const std::string_view name =
        parser::GetNodeTextView(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName), sourceCode);
    return AppendScopedName(name, containers[current], containers, current);
}

uint32_t NamingContext::NextCaller(TSNode node, std::string_view sourceCode, uint32_t containerIdx, uint32_t current)
{
    const std::string_view name =
        parser::GetNodeTextView(parser::GetChildByField(node, parser::GetGrammarSymbols().fieldName), sourceCode);
    return AppendScopedName(name, containers[containerIdx], callers, current);
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
        parser::ForEachNamedChildNode(callee,
                                      [&](TSNode child)
                                      {
                                          if (ts_node_symbol(child) == gs.symIdentifier)
                                          {
                                              last = child;
                                          }
                                      });
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

std::vector<CallSite> CollectCallsFromScopeTree(const Scope& rootScope)
{
    std::vector<CallSite> calls;
    std::vector<const Scope*> worklist;
    worklist.push_back(&rootScope);

    while (!worklist.empty())
    {
        const Scope* scope = worklist.back();
        worklist.pop_back();

        for (const auto& ref : scope->references)
        {
            if (ref.isCall)
            {
                calls.push_back(
                    CallSite{std::string(scope->GetCaller()), ref.name,
                             SourceRange{ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter}});
            }
        }

        for (auto it = scope->children.rbegin(); it != scope->children.rend(); ++it)
        {
            if (it->get())
            {
                worklist.push_back(it->get());
            }
        }
    }

    std::sort(calls.begin(), calls.end(),
              [](const CallSite& a, const CallSite& b) noexcept
              {
                  if (a.range.startLine != b.range.startLine)
                  {
                      return a.range.startLine < b.range.startLine;
                  }
                  return a.range.startCharacter < b.range.startCharacter;
              });

    return calls;
}

void CallGraphIndex::ClearDocumentLocked(const std::string& fileUri)
{
    const auto docIt = m_byDocument.find(fileUri);
    if (docIt == m_byDocument.end())
    {
        return;
    }
    for (const auto& call : docIt->second)
    {
        if (const auto it = m_byCallee.find(call.callee); it != m_byCallee.end())
        {
            it->second.erase(fileUri);
            if (it->second.empty())
            {
                m_byCallee.erase(it);
            }
        }
        if (const auto it = m_byCaller.find(call.caller); it != m_byCaller.end())
        {
            it->second.erase(fileUri);
            if (it->second.empty())
            {
                m_byCaller.erase(it);
            }
        }
    }
    m_byDocument.erase(docIt);
}

void CallGraphIndex::SetDocumentCalls(const std::string& fileUri, std::vector<CallSite> calls)
{
    std::unique_lock lock(m_mutex);
    ClearDocumentLocked(fileUri);
    if (calls.empty())
    {
        return;
    }
    for (const auto& call : calls)
    {
        m_byCallee[call.callee][fileUri].push_back(call);
        m_byCaller[call.caller][fileUri].push_back(call);
    }
    m_byDocument[fileUri] = std::move(calls);
}

void CallGraphIndex::ClearDocument(const std::string& fileUri)
{
    std::unique_lock lock(m_mutex);
    ClearDocumentLocked(fileUri);
}

std::vector<DocumentCalls> CallGraphIndex::FindCallsInIndex(const InvertedIndex& index, std::string_view name) const
{
    std::vector<DocumentCalls> found;
    if (name.empty())
    {
        return found;
    }
    std::shared_lock lock(m_mutex);
    const auto it = index.find(name);
    if (it == index.end())
    {
        return found;
    }
    found.reserve(it->second.size());
    for (const auto& [fileUri, calls] : it->second)
    {
        found.push_back(DocumentCalls{fileUri, calls});
    }
    return found;
}

std::vector<DocumentCalls> CallGraphIndex::FindCallsTo(std::string_view calleeName) const
{
    return FindCallsInIndex(m_byCallee, calleeName);
}

std::vector<DocumentCalls> CallGraphIndex::FindCallsFrom(std::string_view callerQualifiedName) const
{
    return FindCallsInIndex(m_byCaller, callerQualifiedName);
}
} // namespace angel_lsp::analysis
