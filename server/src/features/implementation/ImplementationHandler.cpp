#include "features/implementation/ImplementationHandler.h"
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

        /** @brief The identifier the cursor sits on, or empty when it is not on one. */
        std::string IdentifierAt(const ImplementationRequest &request, TSNode &outNode)
        {
            outNode = TSNode{};
            if (!request.tree)
            {
                return "";
            }

            const TSNode root = ts_tree_root_node(request.tree);
            const TSPoint point{ request.position.line, request.position.character };
            TSNode node = ts_node_descendant_for_point_range(root, point, point);
            if (ts_node_is_null(node) || std::string_view(ts_node_type(node)) != "identifier")
            {
                return "";
            }

            const uint32_t start = ts_node_start_byte(node);
            const uint32_t end = ts_node_end_byte(node);
            if (start >= end || end > request.sourceCode.size())
            {
                return "";
            }

            outNode = node;
            return request.sourceCode.substr(start, end - start);
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

        /**
         * @brief Collects every type that reaches the given one through its declared bases.
         *
         * Walked outward one generation at a time rather than recursively, so a cycle - which the
         * class rules report but do not remove - costs one visit per type instead of hanging the
         * request. Names are compared by their last segment, since a base may be written qualified
         * where the declaration is not, or the other way round.
         */
        std::vector<Symbol> CollectSubtypes(const std::string &rootType, const SymbolTable &table)
        {
            std::vector<std::string> frontier{ LastScopeSegment(rootType) };
            std::vector<std::string> seen{ frontier.front() };
            std::vector<Symbol> subtypes;

            while (!frontier.empty())
            {
                std::vector<std::string> next;

                table.ForEachSymbol([&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.type != SymbolType::Class && sym.type != SymbolType::Interface)
                        {
                            continue;
                        }

                        const std::string bare = LastScopeSegment(sym.name);
                        if (std::find(seen.begin(), seen.end(), bare) != seen.end())
                        {
                            continue;
                        }

                        for (const auto &base : DeclaredBases(sym))
                        {
                            const std::string baseName = LastScopeSegment(analysis::CleanBaseType(base));
                            if (std::find(frontier.begin(), frontier.end(), baseName) == frontier.end())
                            {
                                continue;
                            }

                            seen.push_back(bare);
                            next.push_back(bare);
                            subtypes.push_back(sym);
                            break;
                        }
                    }
                });

                frontier = std::move(next);
            }
            return subtypes;
        }

        lsp::Location ToLocation(const Symbol &sym)
        {
            return lsp::Location{
                lsp::DocumentUri::parse(sym.fileUri),
                lsp::Range{
                    lsp::Position{ sym.startLine, sym.startCharacter },
                    lsp::Position{ sym.endLine, sym.endCharacter }
                }
            };
        }

        /** @brief The type whose body the cursor sits in, or empty when it sits in none. */
        std::string EnclosingType(TSNode node, std::string_view sourceCode)
        {
            for (const auto &container : analysis::GetEnclosingContainers(node, sourceCode))
            {
                if (container.kind == analysis::ContainerKind::Class ||
                    container.kind == analysis::ContainerKind::Interface)
                {
                    return container.name;
                }
            }
            return "";
        }

        /** @brief True when a name is declared as a class or an interface anywhere in the table. */
        bool IsTypeName(const std::string &name, const SymbolTable &table)
        {
            const auto symbols = table.FindSymbolsPtr(name);
            return symbols && std::any_of(symbols->begin(), symbols->end(), [](const Symbol &sym)
            {
                return sym.type == SymbolType::Class || sym.type == SymbolType::Interface;
            });
        }
    }

    std::optional<std::vector<lsp::Location>> GetImplementations(const ImplementationRequest &request)
    {
        TSNode node{};
        const std::string name = IdentifierAt(request, node);
        if (name.empty())
        {
            return std::nullopt;
        }

        const SymbolTable &table = request.symbolTable;

        // The cursor on a type's own name: answer with what derives from it.
        if (IsTypeName(name, table))
        {
            std::vector<lsp::Location> locations;
            for (const auto &sym : CollectSubtypes(name, table))
            {
                locations.push_back(ToLocation(sym));
            }
            return locations.empty() ? std::nullopt : std::optional{ locations };
        }

        // Otherwise the cursor may be on a member of one, in which case the answer is that member
        // as each subtype declares it. Anything else - a local, a global, a call to a free
        // function - has no implementations to speak of, and nullopt says so.
        const std::string owner = EnclosingType(node, request.sourceCode);
        if (owner.empty() || !table.FindSymbolsPtr(owner + "::" + name))
        {
            return std::nullopt;
        }

        std::vector<lsp::Location> locations;
        for (const auto &subtype : CollectSubtypes(owner, table))
        {
            const auto members = table.FindSymbolsPtr(LastScopeSegment(subtype.name) + "::" + name);
            if (!members)
            {
                continue;
            }
            for (const auto &member : *members)
            {
                locations.push_back(ToLocation(member));
            }
        }
        return locations.empty() ? std::nullopt : std::optional{ locations };
    }
}
