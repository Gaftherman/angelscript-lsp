#pragma once

#include "analysis/index/NodeIndex.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "utils/TransparentHash.h"

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
    /**
     * @brief Qualified name of the function the call is written inside, e.g. "Entity::Think".
     */
    std::string caller;

    /**
     * @brief Bare name of what is called, e.g. "Spawn". Never qualified - see the index.
     */
    std::string callee;

    /**
     * @brief Range of the callee's name alone, which is what the protocol asks for.
     */
    SourceRange range;

    bool operator==(const CallSite&) const noexcept = default;
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
 * @brief Collects the calls in one parsed document from an AST tree.
 * @param root Root node of the document's tree.
 * @param sourceCode Text the tree was parsed from.
 * @return Every call in the document, in source order.
 */
std::vector<CallSite> CollectCalls(TSNode root, std::string_view sourceCode);

/**
 * @brief Collects the calls in one parsed document from a NodeIndex.
 * @param index Node index of the document.
 * @param sourceCode Text the document was parsed from.
 * @return Every call in the document, in source order.
 */
std::vector<CallSite> CollectCallsFromIndex(const NodeIndex& index, std::string_view sourceCode);

/**
 * @brief Collects the calls directly from a document's Scope tree without full AST traversal.
 * @param rootScope Root scope of the document.
 * @return Every call in the document, in source order.
 */
std::vector<CallSite> CollectCallsFromScopeTree(const Scope& rootScope);

/**
 * @brief Workspace-wide call index, maintained per document alongside the scope trees.
 *        Answers both directions the call hierarchy needs: who calls this name, and what does this
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
     */
    [[nodiscard]] std::vector<DocumentCalls> FindCallsTo(std::string_view calleeName) const;

    /**
     * @brief Finds every call written inside a function identified by its qualified name.
     * @param[in] callerQualifiedName Fully qualified name of the calling function.
     * @return Vector of DocumentCalls originating from the caller.
     */
    [[nodiscard]] std::vector<DocumentCalls> FindCallsFrom(std::string_view callerQualifiedName) const;

  private:
    void ClearDocumentLocked(const std::string& fileUri);

    using StringDocumentCalls = ankerl::unordered_dense::map<std::string, std::vector<CallSite>>;
    using InvertedIndex =
        ankerl::unordered_dense::map<std::string, StringDocumentCalls, TransparentStringHash, std::equal_to<>>;

    [[nodiscard]] std::vector<DocumentCalls> FindCallsInIndex(const InvertedIndex& index, std::string_view name) const;

    mutable std::shared_mutex m_mutex;
    ankerl::unordered_dense::map<std::string, std::vector<CallSite>> m_byDocument;
    InvertedIndex m_byCallee;
    InvertedIndex m_byCaller;
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
