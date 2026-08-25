#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <string_view>

namespace angel_lsp::features
{
    namespace
    {
        using analysis::Symbol;
        using analysis::SymbolTable;
        using analysis::SymbolType;

        /** @brief Strips a namespace qualification, leaving the last segment ("G::A" -> "A"). */
        std::string LastScopeSegment(const std::string &name)
        {
            const size_t pos = name.rfind("::");
            return pos == std::string::npos ? name : name.substr(pos + 2);
        }

        bool IsTypeSymbol(const Symbol &sym)
        {
            return sym.type == SymbolType::Class || sym.type == SymbolType::Interface;
        }

        /** @brief Every base a declaration lists, whichever kind of declaration it is. */
        std::vector<std::string> DeclaredBases(const Symbol &sym)
        {
            if (sym.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(sym.signature))
            {
                return sym.GetClass().bases;
            }
            if (sym.type == SymbolType::Interface && std::holds_alternative<analysis::InterfaceSignature>(sym.signature))
            {
                return sym.GetInterface().inheritedInterfaces;
            }
            return {};
        }

        lsp::Range ToRange(const analysis::SourceRange &range)
        {
            return lsp::Range{
                lsp::Position{ range.startLine, range.startCharacter },
                lsp::Position{ range.endLine, range.endCharacter }
            };
        }

        lsp::TypeHierarchyItem ToItem(const Symbol &sym)
        {
            lsp::TypeHierarchyItem item;
            item.name = LastScopeSegment(sym.name);
            item.kind = sym.type == SymbolType::Interface ? lsp::SymbolKind::Interface : lsp::SymbolKind::Class;
            item.uri = lsp::DocumentUri::parse(sym.fileUri);

            // fullRange covers the declaration and its body; selectionRange is the name alone. The
            // protocol requires the second be contained by the first, and a collector that recorded
            // neither would give both as an empty range at the origin - so the symbol's own
            // start/end is the fallback, which is always at least the name.
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
            return item;
        }

        /** @brief The type declarations a bare name resolves to, if any. */
        std::vector<Symbol> FindTypeDeclarations(const std::string &name, const SymbolTable &table)
        {
            std::vector<Symbol> found;
            const auto symbols = table.FindSymbolsPtr(name);
            if (!symbols)
            {
                return found;
            }
            for (const auto &sym : *symbols)
            {
                if (IsTypeSymbol(sym))
                {
                    found.push_back(sym);
                }
            }
            return found;
        }

        /** @brief The identifier the cursor sits on, or empty when it is not on one. */
        std::string IdentifierAt(const TypeHierarchyPrepareRequest &request, TSNode &outNode)
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
            if (std::string_view(ts_node_type(node)) != "identifier")
            {
                return "";
            }

            const uint32_t start = ts_node_start_byte(node);
            const uint32_t end = ts_node_end_byte(node);
            if (start >= end || end > request.sourceCode.size())
            {
                return "";
            }
            return request.sourceCode.substr(start, end - start);
        }
    }

    std::optional<std::vector<lsp::TypeHierarchyItem>> PrepareTypeHierarchy(const TypeHierarchyPrepareRequest &request)
    {
        TSNode node{};
        const std::string name = IdentifierAt(request, node);

        std::vector<Symbol> declarations;
        if (!name.empty())
        {
            declarations = FindTypeDeclarations(name, request.symbolTable);
        }

        // Not on a type's name, but perhaps inside one's body - which is where a reader asking for
        // the hierarchy usually has the cursor.
        if (declarations.empty() && !ts_node_is_null(node))
        {
            for (const auto &container : analysis::GetEnclosingContainers(node, request.sourceCode))
            {
                if (container.kind != analysis::ContainerKind::Class &&
                    container.kind != analysis::ContainerKind::Interface)
                {
                    continue;
                }
                declarations = FindTypeDeclarations(container.name, request.symbolTable);
                break;
            }
        }

        if (declarations.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::TypeHierarchyItem> items;
        items.reserve(declarations.size());
        for (const auto &sym : declarations)
        {
            items.push_back(ToItem(sym));
        }
        return items;
    }

    std::optional<std::vector<lsp::TypeHierarchyItem>> GetSupertypes(const TypeHierarchyItemRequest &request)
    {
        const auto declarations = FindTypeDeclarations(request.item.name, request.symbolTable);
        if (declarations.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::TypeHierarchyItem> items;
        std::vector<std::string> seen;

        for (const auto &declaration : declarations)
        {
            for (const auto &base : DeclaredBases(declaration))
            {
                const std::string baseName = LastScopeSegment(analysis::CleanBaseType(base));
                if (baseName.empty() || std::find(seen.begin(), seen.end(), baseName) != seen.end())
                {
                    continue;
                }
                seen.push_back(baseName);

                // A base that resolves to nothing is an engine-registered type, and there is no
                // declaration to point the client at - so it is left out rather than offered as an
                // item that navigates nowhere.
                for (const auto &baseSymbol : FindTypeDeclarations(baseName, request.symbolTable))
                {
                    items.push_back(ToItem(baseSymbol));
                }
            }
        }
        return items.empty() ? std::nullopt : std::optional{ items };
    }

    std::optional<std::vector<lsp::TypeHierarchyItem>> GetSubtypes(const TypeHierarchyItemRequest &request)
    {
        const std::string target = LastScopeSegment(request.item.name);
        if (target.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::TypeHierarchyItem> items;
        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (!IsTypeSymbol(sym) || LastScopeSegment(sym.name) == target)
                {
                    continue;
                }

                for (const auto &base : DeclaredBases(sym))
                {
                    if (LastScopeSegment(analysis::CleanBaseType(base)) == target)
                    {
                        items.push_back(ToItem(sym));
                        break;
                    }
                }
            }
        });

        return items.empty() ? std::nullopt : std::optional{ items };
    }
}
