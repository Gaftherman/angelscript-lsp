#pragma once

#include "analysis/SymbolTable.h"

#include <ankerl/unordered_dense.h>
#include <memory>
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

        /** @brief Replaces everything recorded for one document. */
        void SetDocumentCalls(const std::string &fileUri, std::vector<CallSite> calls);

        /** @brief Discards everything recorded for one document. */
        void ClearDocument(const std::string &fileUri);

        /**
         * @brief Every call to a given bare name, across every indexed document.
         * @note Returned by value: the index is written from the analysis thread while requests
         *       read it, and a snapshot is cheaper to reason about than a lock held across a
         *       whole request.
         */
        std::vector<DocumentCalls> FindCallsTo(const std::string &calleeName) const;

        /** @brief Every call written inside one function, by its qualified name. */
        std::vector<DocumentCalls> FindCallsFrom(const std::string &callerQualifiedName) const;

    private:
        mutable std::shared_mutex m_mutex;
        ankerl::unordered_dense::map<std::string, std::vector<CallSite>> m_byDocument;
    };
}
