#include "analysis/CallGraph.h"
#include "analysis/ASTUtils.h"

#include <string>
#include <utility>

namespace angel_lsp::analysis
{
    namespace
    {
        /**
         * @brief Node text as an owning string.
         *
         * Kept per translation unit rather than shared with ASTUtils::NodeText, which returns a
         * string_view. The two are not interchangeable: callers here store the result, concatenate
         * it, and use it after the node has gone out of scope, so handing them a view would trade a
         * duplicated three-line function for a lifetime question at several dozen call sites.
         * Deduplicating it was attempted and reverted for exactly that reason.
         */
        std::string NodeText(TSNode node, std::string_view sourceCode)
        {
            if (ts_node_is_null(node))
            {
                return "";
            }

            const uint32_t start = ts_node_start_byte(node);
            const uint32_t end = ts_node_end_byte(node);
            if (start >= end || end > sourceCode.size())
            {
                return "";
            }
            return std::string(sourceCode.substr(start, end - start));
        }

        constexpr uint32_t k_functionFieldLength = 8; ///< "function"
        constexpr uint32_t k_memberFieldLength = 6;   ///< "member"
        constexpr uint32_t k_nameFieldLength = 4;     ///< "name"

        SourceRange ToSourceRange(TSNode node)
        {
            const TSPoint start = ts_node_start_point(node);
            const TSPoint end = ts_node_end_point(node);
            return SourceRange{ start.row, start.column, end.row, end.column };
        }

        /** @brief The node naming what a call reaches, or a null node when it names nothing stable. */
        TSNode CalleeNameNode(TSNode callNode)
        {
            TSNode callee = ts_node_child_by_field_name(callNode, "function", k_functionFieldLength);
            if (ts_node_is_null(callee))
            {
                return TSNode{};
            }

            const std::string_view calleeType = ts_node_type(callee);

            if (calleeType == "member_expression")
            {
                return ts_node_child_by_field_name(callee, "member", k_memberFieldLength);
            }

            if (calleeType == "scoped_identifier")
            {
                // The last segment, so `Game::Spawn()` and `Spawn()` are recorded under one name.
                // Resolving which declaration that reaches is the hierarchy's job, not the index's.
                TSNode last{};
                const uint32_t count = ts_node_named_child_count(callee);
                for (uint32_t i = 0; i < count; ++i)
                {
                    TSNode child = ts_node_named_child(callee, i);
                    if (std::string_view(ts_node_type(child)) == "identifier")
                    {
                        last = child;
                    }
                }
                return last;
            }

            if (calleeType == "identifier")
            {
                return callee;
            }

            // A lambda, an indexed value, a call through a returned handle: nothing with a name to
            // record, and inventing one would put a call in the index that no user can navigate to.
            return TSNode{};
        }

        /**
         * @brief Recursive descent carrying the container path and the enclosing function's name.
         *
         * The two names are passed by reference and only rebuilt at the handful of nodes that
         * change them. Copying them into locals at every node instead - which is the obvious way to
         * write this - cost two string copies per node of the tree, a third of the pass's whole
         * running time, for work that changes at a few dozen nodes per file. Measured: 3.30 ms per
         * file that way against 2.19 this way, over the 300-file sample in RuleCostTest.
         */
        void Walk(TSNode node,
                  std::string_view sourceCode,
                  const std::string &containerPath,
                  const std::string &caller,
                  std::vector<CallSite> &out,
                  int depth = 0)
        {
            // See k_maxAstDepth in ASTUtils.h.
            if (depth > k_maxAstDepth)
                return;

            const std::string_view nodeType = ts_node_type(node);

            std::string rebuilt;
            const std::string *nextContainer = &containerPath;
            const std::string *nextCaller = &caller;

            if (nodeType == "class_declaration" || nodeType == "interface_declaration" ||
                nodeType == "mixin_declaration" || nodeType == "namespace_declaration")
            {
                const std::string name = NodeText(ts_node_child_by_field_name(node, "name", k_nameFieldLength), sourceCode);
                if (!name.empty())
                {
                    rebuilt = containerPath.empty() ? name : containerPath + "::" + name;
                    nextContainer = &rebuilt;
                }
            }
            else if (nodeType == "func_declaration")
            {
                const std::string name = NodeText(ts_node_child_by_field_name(node, "name", k_nameFieldLength), sourceCode);
                if (!name.empty())
                {
                    rebuilt = containerPath.empty() ? name : containerPath + "::" + name;
                    nextCaller = &rebuilt;
                }
            }
            else if (nodeType == "call_expression")
            {
                const TSNode nameNode = CalleeNameNode(node);
                if (!ts_node_is_null(nameNode))
                {
                    std::string callee = NodeText(nameNode, sourceCode);
                    if (!callee.empty())
                    {
                        out.push_back(CallSite{ caller, std::move(callee), ToSourceRange(nameNode) });
                    }
                }
                // Falls through to the children: arguments carry calls of their own.
            }

            const uint32_t childCount = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < childCount; ++i)
            {
                Walk(ts_node_named_child(node, i), sourceCode, *nextContainer, *nextCaller, out, depth + 1);
            }
        }
    }

    std::vector<CallSite> CollectCalls(TSNode root, std::string_view sourceCode)
    {
        std::vector<CallSite> calls;
        if (ts_node_is_null(root) || sourceCode.empty())
        {
            return calls;
        }

        Walk(root, sourceCode, "", "", calls);
        return calls;
    }

    void CallGraphIndex::SetDocumentCalls(const std::string &fileUri, std::vector<CallSite> calls)
    {
        std::unique_lock lock(m_mutex);
        if (calls.empty())
        {
            m_byDocument.erase(fileUri);
            return;
        }
        m_byDocument[fileUri] = std::move(calls);
    }

    void CallGraphIndex::ClearDocument(const std::string &fileUri)
    {
        std::unique_lock lock(m_mutex);
        m_byDocument.erase(fileUri);
    }

    std::vector<DocumentCalls> CallGraphIndex::FindCallsTo(const std::string &calleeName) const
    {
        std::vector<DocumentCalls> found;
        if (calleeName.empty())
        {
            return found;
        }

        std::shared_lock lock(m_mutex);
        for (const auto &[fileUri, calls] : m_byDocument)
        {
            DocumentCalls matches{ fileUri, {} };
            for (const auto &call : calls)
            {
                if (call.callee == calleeName)
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

    std::vector<DocumentCalls> CallGraphIndex::FindCallsFrom(const std::string &callerQualifiedName) const
    {
        std::vector<DocumentCalls> found;
        if (callerQualifiedName.empty())
        {
            return found;
        }

        std::shared_lock lock(m_mutex);
        for (const auto &[fileUri, calls] : m_byDocument)
        {
            DocumentCalls matches{ fileUri, {} };
            for (const auto &call : calls)
            {
                if (call.caller == callerQualifiedName)
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
}
