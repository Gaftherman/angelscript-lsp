#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace angel_lsp::features
{
    namespace
    {
        using analysis::CallSite;
        using analysis::Symbol;
        using analysis::SymbolTable;
        using analysis::SymbolType;

        constexpr uint32_t k_nameFieldLength = 4; ///< "name"

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

        lsp::Range ToRange(const analysis::SourceRange &range)
        {
            return lsp::Range{
                lsp::Position{ range.startLine, range.startCharacter },
                lsp::Position{ range.endLine, range.endCharacter }
            };
        }

        lsp::CallHierarchyItem ToItem(const Symbol &sym)
        {
            lsp::CallHierarchyItem item;
            item.name = analysis::LastScopeSegment(sym.name);
            item.kind = sym.containerName.empty() ? lsp::SymbolKind::Function : lsp::SymbolKind::Method;
            item.uri = lsp::DocumentUri::parse(sym.fileUri);

            const bool hasFullRange = sym.fullRange.endLine != 0 || sym.fullRange.endCharacter != 0;
            item.range = hasFullRange
                             ? ToRange(sym.fullRange)
                             : lsp::Range{ lsp::Position{ sym.startLine, sym.startCharacter },
                                           lsp::Position{ sym.endLine, sym.endCharacter } };

            const bool hasSelection = sym.selectionRange.endLine != 0 || sym.selectionRange.endCharacter != 0;
            item.selectionRange = hasSelection
                                      ? ToRange(sym.selectionRange)
                                      : lsp::Range{ lsp::Position{ sym.startLine, sym.startCharacter },
                                                    lsp::Position{ sym.endLine, sym.endCharacter } };

            // The qualified name is what the follow-up requests need and what the item alone cannot
            // carry: `Think` is a name a dozen classes have. `data` exists exactly to be handed back
            // untouched between prepare and the calls requests, so it is where this belongs.
            const std::string qualified = sym.qualifiedName.empty() ? sym.name : sym.qualifiedName;
            item.detail = qualified;
            item.data = lsp::LSPAny(std::string(qualified));
            return item;
        }

        /** @brief The qualified name a follow-up request is about, from the item it was handed. */
        std::string QualifiedNameOf(const lsp::CallHierarchyItem &item)
        {
            if (item.data.has_value() && item.data->isString())
            {
                return item.data->string();
            }
            // A client that dropped `data` still gets an answer, just a broader one.
            return item.name;
        }

        bool IsFunctionSymbol(const Symbol &sym)
        {
            return sym.type == SymbolType::Function &&
                   std::holds_alternative<analysis::FunctionSignature>(sym.signature);
        }

        /** @brief Function declarations whose qualified name is exactly this. */
        std::vector<Symbol> FindByQualifiedName(const std::string &qualifiedName, const SymbolTable &table)
        {
            std::vector<Symbol> found;
            const auto symbols = table.FindSymbolsPtr(qualifiedName);
            if (!symbols)
            {
                return found;
            }
            for (const auto &sym : *symbols)
            {
                if (IsFunctionSymbol(sym))
                {
                    found.push_back(sym);
                }
            }
            return found;
        }

        /**
         * @brief Function declarations whose bare name is this, wherever they were declared.
         *
         * A call site records `Think`, and the table is keyed by `Entity::Think` - so a bare name
         * costs a full walk. Affordable because a hierarchy request is something the user asked
         * for once, not work done on every keystroke; the per-edit passes all read the RuleIndex
         * instead, which is built for exactly that reason.
         */
        std::vector<Symbol> FindByBareName(const std::string &bareName, const SymbolTable &table)
        {
            std::vector<Symbol> found;
            if (bareName.empty())
            {
                return found;
            }

            table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (IsFunctionSymbol(sym) && analysis::LastScopeSegment(sym.name) == bareName)
                    {
                        found.push_back(sym);
                    }
                }
            });
            return found;
        }

        /** @brief The function whose body a node sits in, qualified, or empty when it sits in none. */
        std::string EnclosingFunction(TSNode node, std::string_view sourceCode)
        {
            TSNode owner = node;
            while (!ts_node_is_null(owner) && std::string_view(ts_node_type(owner)) != "func_declaration")
            {
                owner = ts_node_parent(owner);
            }
            if (ts_node_is_null(owner))
            {
                return "";
            }

            const std::string name = NodeText(ts_node_child_by_field_name(owner, "name", k_nameFieldLength), sourceCode);
            if (name.empty())
            {
                return "";
            }

            for (const auto &container : analysis::GetEnclosingContainers(owner, sourceCode))
            {
                return container.qualifiedName.empty() ? name : container.qualifiedName + "::" + name;
            }
            return name;
        }

        /** @brief The identifier the cursor sits on, or empty when it is not on one. */
        std::string IdentifierAt(const CallHierarchyPrepareRequest &request, TSNode &outNode)
        {
            outNode = TSNode{};
            if (!request.tree)
            {
                return "";
            }

            const TSNode root = ts_tree_root_node(request.tree);
            const TSPoint point{ request.position.line, request.position.character };
            TSNode node = ts_node_descendant_for_point_range(root, point, point);
            if (ts_node_is_null(node))
            {
                return "";
            }

            outNode = node;
            return std::string_view(ts_node_type(node)) == "identifier" ? NodeText(node, request.sourceCode) : std::string();
        }
    }

    std::optional<std::vector<lsp::CallHierarchyItem>> PrepareCallHierarchy(const CallHierarchyPrepareRequest &request)
    {
        TSNode node{};
        const std::string name = IdentifierAt(request, node);

        std::vector<Symbol> declarations;
        if (!name.empty())
        {
            declarations = FindByBareName(name, request.symbolTable);
        }

        // Not on a function's name, but perhaps inside one's body - which is where a reader asking
        // "who calls this" usually has the cursor.
        if (declarations.empty() && !ts_node_is_null(node))
        {
            const std::string enclosing = EnclosingFunction(node, request.sourceCode);
            if (!enclosing.empty())
            {
                declarations = FindByQualifiedName(enclosing, request.symbolTable);
            }
        }

        if (declarations.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::CallHierarchyItem> items;
        items.reserve(declarations.size());
        for (const auto &sym : declarations)
        {
            items.push_back(ToItem(sym));
        }
        return items;
    }

    std::optional<std::vector<lsp::CallHierarchyIncomingCall>> GetIncomingCalls(const CallHierarchyItemRequest &request)
    {
        const std::string target = analysis::LastScopeSegment(request.item.name);
        if (target.empty())
        {
            return std::nullopt;
        }

        // Gathered per calling function, since the protocol asks for one entry carrying every range
        // at which that caller writes the call.
        std::vector<std::pair<std::string, std::vector<lsp::Range>>> byCaller;

        for (const auto &document : request.callGraph.FindCallsTo(target))
        {
            for (const auto &call : document.calls)
            {
                if (call.caller.empty())
                {
                    // Written outside any function - a global initializer, an enum value. There is
                    // no caller to make an item of.
                    continue;
                }

                auto existing = std::find_if(byCaller.begin(), byCaller.end(),
                                             [&call](const auto &entry) { return entry.first == call.caller; });
                if (existing == byCaller.end())
                {
                    byCaller.emplace_back(call.caller, std::vector<lsp::Range>{ ToRange(call.range) });
                }
                else
                {
                    existing->second.push_back(ToRange(call.range));
                }
            }
        }

        std::vector<lsp::CallHierarchyIncomingCall> incoming;
        for (auto &[caller, ranges] : byCaller)
        {
            for (const auto &sym : FindByQualifiedName(caller, request.symbolTable))
            {
                lsp::CallHierarchyIncomingCall entry;
                entry.from = ToItem(sym);
                entry.fromRanges = ranges;
                incoming.push_back(std::move(entry));
            }
        }

        return incoming.empty() ? std::nullopt : std::optional{ incoming };
    }

    std::optional<std::vector<lsp::CallHierarchyOutgoingCall>> GetOutgoingCalls(const CallHierarchyItemRequest &request)
    {
        const std::string caller = QualifiedNameOf(request.item);
        if (caller.empty())
        {
            return std::nullopt;
        }

        std::vector<std::pair<std::string, std::vector<lsp::Range>>> byCallee;

        for (const auto &document : request.callGraph.FindCallsFrom(caller))
        {
            for (const auto &call : document.calls)
            {
                auto existing = std::find_if(byCallee.begin(), byCallee.end(),
                                             [&call](const auto &entry) { return entry.first == call.callee; });
                if (existing == byCallee.end())
                {
                    byCallee.emplace_back(call.callee, std::vector<lsp::Range>{ ToRange(call.range) });
                }
                else
                {
                    existing->second.push_back(ToRange(call.range));
                }
            }
        }

        std::vector<lsp::CallHierarchyOutgoingCall> outgoing;
        for (auto &[callee, ranges] : byCallee)
        {
            // A callee that resolves to no declaration is an engine-registered function. There is
            // nowhere to navigate to, so it is left out rather than offered as a dead entry.
            for (const auto &sym : FindByBareName(callee, request.symbolTable))
            {
                lsp::CallHierarchyOutgoingCall entry;
                entry.to = ToItem(sym);
                entry.fromRanges = ranges;
                outgoing.push_back(std::move(entry));
            }
        }

        return outgoing.empty() ? std::nullopt : std::optional{ outgoing };
    }
}
