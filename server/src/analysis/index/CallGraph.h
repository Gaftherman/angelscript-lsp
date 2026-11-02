#pragma once

#include "analysis/index/SymbolTable.h"

#include <ankerl/unordered_dense.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tree_sitter/api.h>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief One call written in a document: who wrote it, what it names, and where.
 */
struct CallSite
{
    /** @brief Qualified name of the function the call is written inside, e.g. "Entity::Think". */
    std::string caller;

    /** @brief Bare name of what is called, e.g. "Spawn". Never qualified - see the index. */
    std::string callee;

    /** @brief Range of the callee's name alone, which is what the protocol asks for. */
    SourceRange range;
};

/**
 * @brief Every call written in one document.
 */
struct DocumentCalls
{
    std::string fileUri;
    std::vector<CallSite> calls;
};

/**
 * @brief Collects the calls in one parsed document.
 *
 * Kept out of SymbolCollector deliberately. The symbol table records declarations, and a call
 * is not one - CallReference exists there only for the handful of calls written outside any
 * function body, and every rule that walks the table has to filter it out already. A corpus
 * file carries hundreds of calls, so folding them in would grow the table by an order of
 * magnitude and slow down every rule to serve one feature.
 *
 * Names are recorded bare and unresolved. A call site says `Think()` or `e.Think()`, and which
 * declaration that reaches needs the receiver's type - a question the call hierarchy answers
 * later, against the symbol table, and only for the one item the user asked about.
 *
 * @param root Root node of the document's tree.
 * @param sourceCode Text the tree was parsed from.
 * @return Every call in the document, in source order.
 */
std::vector<CallSite> CollectCalls(TSNode root, std::string_view sourceCode);

/**
 * @brief Workspace-wide call index, maintained per document alongside the scope trees.
 *
 * Answers both directions the call hierarchy needs: who calls this name, and what does this
 * function call. Held separately from the SymbolTable so that its size and its lifetime are
 * its own, and so that no declaration rule pays for it.
 */
class CallGraphIndex
{
  public:
    CallGraphIndex() = default;

    /**
     * @brief Replaces all calls recorded for the specified document URI.
     * @param[in] fileUri URI of the document being updated.
     * @param[in] calls Vector of call sites identified in the document.
     */
    void SetDocumentCalls(const std::string& fileUri, std::vector<CallSite> calls);

    /**
     * @brief Discards all calls recorded for the specified document URI.
     * @param[in] fileUri URI of the document to clear.
     */
    void ClearDocument(const std::string& fileUri);

    /**
     * @brief Finds every call to a given bare name across every indexed document.
     * @param[in] calleeName Bare name of the callee function to look for.
     * @return Vector of DocumentCalls matching the callee name.
     * @note Returned by value: the index is written from the analysis thread while requests
     *       read it, and a snapshot is cheaper to reason about than a lock held across a
     *       whole request.
     */
    std::vector<DocumentCalls> FindCallsTo(const std::string& calleeName) const;

    /**
     * @brief Finds every call written inside a function identified by its qualified name.
     * @param[in] callerQualifiedName Fully qualified name of the calling function.
     * @return Vector of DocumentCalls originating from the caller.
     */
    std::vector<DocumentCalls> FindCallsFrom(const std::string& callerQualifiedName) const;

  private:
    mutable std::shared_mutex m_mutex;
    ankerl::unordered_dense::map<std::string, std::vector<CallSite>> m_byDocument;
};

/**
 * @brief Queue item tracking call graph traversal paths and call hierarchy depth.
 */
struct TraversalItem
{
    TSNode node{};
    uint32_t containerIndex = 0;
    uint32_t callerIndex = 0;
};

/**
 * @brief Context for resolving fully-qualified callable names and enclosing class/namespace scopes in call graphs.
 */
struct NamingContext
{
    std::vector<std::string> containers{""};
    std::vector<std::string> callers{""};

    /**
     * @brief Computes the next container qualified name index for an AST container node.
     * @param[in] node AST node representing the enclosing container.
     * @param[in] sourceCode Source text buffer of the document.
     * @param[in] current Current container index in the naming context.
     * @return Updated container index in the naming context.
     */
    uint32_t NextContainer(TSNode node, std::string_view sourceCode, uint32_t current);

    /**
     * @brief Computes the next caller qualified name index for an AST function node.
     * @param[in] node AST node representing the function declaration.
     * @param[in] sourceCode Source text buffer of the document.
     * @param[in] containerIdx Index of the enclosing container.
     * @param[in] current Current caller index in the naming context.
     * @return Updated caller index in the naming context.
     */
    uint32_t NextCaller(TSNode node, std::string_view sourceCode, uint32_t containerIdx, uint32_t current);
};
} // namespace angel_lsp::analysis
