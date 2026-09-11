#include "features/implementation/ImplementationHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/rules/RuleIndex.h"
#include "parser/GrammarNames.h"
#include "utils/LspLogger.h"
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string_view>

namespace angel_lsp::features
{
    namespace
    {
        using analysis::Symbol;
        using analysis::SymbolTable;
        using analysis::SymbolType;

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
            const auto ruleIndex = table.GetRuleIndex();
            if (ruleIndex)
            {
                std::string bareRoot = analysis::LastScopeSegment(rootType);
                std::vector<std::string> frontier{ bareRoot };
                if (bareRoot != rootType && !rootType.empty())
                {
                    frontier.push_back(rootType);
                }
                ankerl::unordered_dense::set<std::string> seen{ bareRoot, rootType };
                std::vector<Symbol> subtypes;

                while (!frontier.empty())
                {
                    std::vector<std::string> next;

                    for (const auto &baseName : frontier)
                    {
                        const auto it = ruleIndex->derivedByBase.find(baseName);
                        if (it == ruleIndex->derivedByBase.end())
                        {
                            continue;
                        }

                        for (const auto &derived : it->second)
                        {
                            const std::string bare = analysis::LastScopeSegment(derived.name);
                            if (!seen.insert(bare).second)
                            {
                                continue;
                            }
                            if (!derived.qualifiedName.empty())
                            {
                                seen.insert(derived.qualifiedName);
                            }

                            next.push_back(bare);
                            if (!derived.qualifiedName.empty() && derived.qualifiedName != bare)
                            {
                                next.push_back(derived.qualifiedName);
                            }

                            const auto symList = table.FindSymbolsPtr(derived.qualifiedName.empty() ? derived.name : derived.qualifiedName);
                            if (symList)
                            {
                                for (const auto &s : *symList)
                                {
                                    if (s.type == SymbolType::Class || s.type == SymbolType::Interface)
                                    {
                                        subtypes.push_back(s);
                                    }
                                }
                            }
                        }
                    }

                    frontier = std::move(next);
                }
                return subtypes;
            }

            std::vector<std::string> frontier{ analysis::LastScopeSegment(rootType) };
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

                        const std::string bare = analysis::LastScopeSegment(sym.name);
                        if (std::find(seen.begin(), seen.end(), bare) != seen.end())
                        {
                            continue;
                        }

                        for (const auto &base : DeclaredBases(sym))
                        {
                            const std::string baseName = analysis::LastScopeSegment(analysis::CleanBaseType(base));
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
            uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
            uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
            uint32_t eL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endLine : sym.endLine;
            uint32_t eC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.endCharacter : sym.endCharacter;

            return lsp::Location{
                lsp::DocumentUri::parse(sym.fileUri),
                lsp::Range{
                    lsp::Position{ sL, sC },
                    lsp::Position{ eL, eC }
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

        if (request.logger && request.logger->IsDebugEnabled())
        {
            request.logger->LogDebug(fmt::format("[Implementation] Resolving implementations for '{}' at {}:{} in {}",
                name, request.position.line, request.position.character, request.uri));
        }

        const SymbolTable &table = request.symbolTable;

        // 1. The cursor on a type's own name: answer with what derives from it, falling back to definition.
        if (IsTypeName(name, table))
        {
            std::vector<lsp::Location> locations;
            for (const auto &sym : CollectSubtypes(name, table))
            {
                locations.push_back(ToLocation(sym));
            }
            if (locations.empty())
            {
                const auto typeSyms = table.FindSymbolsPtr(name);
                if (typeSyms)
                {
                    for (const auto &sym : *typeSyms)
                    {
                        if (sym.type == SymbolType::Class || sym.type == SymbolType::Interface)
                        {
                            locations.push_back(ToLocation(sym));
                        }
                    }
                }
            }
            return locations.empty() ? std::nullopt : std::optional{ locations };
        }

        // 2. Member method or field
        std::string owner = EnclosingType(node, request.sourceCode);
        if (owner.empty())
        {
            TSNode p = ts_node_parent(node);
            if (!ts_node_is_null(p) && std::string_view(ts_node_type(p)) == "member_expression")
            {
                TSNode objNode = parser::GetChildByField(p, parser::fields::Object);
                if (!ts_node_is_null(objNode))
                {
                    std::string objType = analysis::ResolveExpressionType(objNode, nullptr, table, request.sourceCode, request.uri);
                    owner = analysis::CleanBaseType(objType);
                }
            }
        }

        if (!owner.empty())
        {
            auto memberSyms = table.FindSymbolsPtr(owner + "::" + name);
            if (!memberSyms || memberSyms->empty())
            {
                auto ownerSyms = table.FindSymbolsPtr(owner);
                if (ownerSyms)
                {
                    for (const auto &os : *ownerSyms)
                    {
                        if (os.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(os.signature))
                        {
                            const auto &cls = os.GetClass();
                            for (const auto &m : cls.includedMixins)
                            {
                                auto mSyms = table.FindSymbolsPtr(analysis::LastScopeSegment(m) + "::" + name);
                                if (mSyms && !mSyms->empty())
                                {
                                    memberSyms = mSyms;
                                    break;
                                }
                            }
                            if (memberSyms && !memberSyms->empty())
                            {
                                break;
                            }
                            for (const auto &b : cls.bases)
                            {
                                std::string cleanB = analysis::CleanBaseType(b);
                                auto mSyms = table.FindSymbolsPtr(analysis::LastScopeSegment(cleanB) + "::" + name);
                                if (mSyms && !mSyms->empty())
                                {
                                    memberSyms = mSyms;
                                    break;
                                }
                            }
                            if (memberSyms && !memberSyms->empty())
                            {
                                break;
                            }
                        }
                    }
                }
            }

            if (memberSyms && !memberSyms->empty())
            {
                std::vector<lsp::Location> locations;
                ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenLocs;
                auto addLoc = [&](const Symbol &sym)
                {
                    uint32_t sL = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startLine : sym.startLine;
                    uint32_t sC = (sym.selectionRange.endLine > 0 || sym.selectionRange.endCharacter > 0) ? sym.selectionRange.startCharacter : sym.startCharacter;
                    uint64_t key = (static_cast<uint64_t>(sL) << 32) | sC;
                    if (seenLocs.insert({ sym.fileUri, key }).second)
                    {
                        locations.push_back(ToLocation(sym));
                    }
                };

                for (const auto &subtype : CollectSubtypes(owner, table))
                {
                    const auto members = table.FindSymbolsPtr(analysis::LastScopeSegment(subtype.name) + "::" + name);
                    if (members && !members->empty())
                    {
                        for (const auto &member : *members)
                        {
                            addLoc(member);
                        }
                    }
                    else if (subtype.type == SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(subtype.signature))
                    {
                        const auto &cls = subtype.GetClass();
                        for (const auto &m : cls.includedMixins)
                        {
                            const auto mixinMembers = table.FindSymbolsPtr(analysis::LastScopeSegment(m) + "::" + name);
                            if (mixinMembers)
                            {
                                for (const auto &mixMember : *mixinMembers)
                                {
                                    addLoc(mixMember);
                                }
                            }
                        }
                        for (const auto &b : cls.bases)
                        {
                            std::string cleanB = analysis::CleanBaseType(b);
                            const auto mixinMembers = table.FindSymbolsPtr(analysis::LastScopeSegment(cleanB) + "::" + name);
                            if (mixinMembers)
                            {
                                for (const auto &mixMember : *mixinMembers)
                                {
                                    addLoc(mixMember);
                                }
                            }
                        }
                    }
                }
                if (locations.empty())
                {
                    // Fallback to definition of target member when no overrides/subtypes exist
                    for (const auto &member : *memberSyms)
                    {
                        addLoc(member);
                    }
                }
                return locations.empty() ? std::nullopt : std::optional{ locations };
            }
        }

        // 3. Free function fallback
        const auto globalSyms = table.FindSymbolsPtr(name);
        if (globalSyms && !globalSyms->empty())
        {
            std::vector<lsp::Location> locations;
            for (const auto &sym : *globalSyms)
            {
                if (sym.type == SymbolType::Function)
                {
                    locations.push_back(ToLocation(sym));
                }
            }
            if (!locations.empty())
            {
                return locations;
            }
        }

        return std::nullopt;
    }
}
