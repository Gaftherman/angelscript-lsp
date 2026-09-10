#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"

#include <algorithm>
#include <string>
#include <vector>
#include <utility>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::features
{
    namespace
    {
        struct RangeKey
        {
            uint32_t startLine = 0;
            uint32_t startCharacter = 0;
            uint32_t endLine = 0;
            uint32_t endCharacter = 0;

            bool operator==(const RangeKey &other) const noexcept
            {
                return startLine == other.startLine &&
                       startCharacter == other.startCharacter &&
                       endLine == other.endLine &&
                       endCharacter == other.endCharacter;
            }
        };

        struct RangeKeyHash
        {
            size_t operator()(const RangeKey &k) const noexcept
            {
                size_t h = std::hash<uint32_t>{}(k.startLine);
                h ^= std::hash<uint32_t>{}(k.startCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(k.endLine) + 0x9e3779b9 + (h << 6) + (h >> 2);
                h ^= std::hash<uint32_t>{}(k.endCharacter) + 0x9e3779b9 + (h << 6) + (h >> 2);
                return h;
            }
        };

        std::string GetEnclosingClassName(const analysis::SymbolTable &symbolTable, const std::string &uri, uint32_t line)
        {
            std::string enclosingClass;
            symbolTable.ForEachSymbolInFile(
                uri,
                [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if ((sym.type == analysis::SymbolType::Class || sym.type == analysis::SymbolType::Interface) &&
                            sym.fileUri == uri)
                        {
                            if (line >= sym.startLine && line <= sym.endLine)
                            {
                                enclosingClass = sym.name;
                            }
                        }
                    }
                });
            return enclosingClass;
        }

        /**
         * @brief Recursively traverses lexical scopes to collect unique call-site references to a symbol group.
         * @param fileUri URI of document owning the scope tree.
         * @param scope Current scope to check.
         * @param targetName Symbol name to match.
         * @param group Symbols sharing this declaration range.
         * @param compatibleClasses Set of class names compatible with the symbol group (for class members).
         * @param symbolTable Global symbol table.
         * @param request Immutable CodeLens request.
         * @param seenRefs Set of unique reference locations across documents.
         */
        void CollectReferencesInScope(const std::string &fileUri,
                                     const analysis::Scope *scope,
                                     const std::string &targetName,
                                     const std::vector<analysis::Symbol> &group,
                                     const ankerl::unordered_dense::set<std::string> &compatibleClasses,
                                     const analysis::SymbolTable &symbolTable,
                                     const CodeLensRequest &request,
                                     ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> &seenRefs)
        {
            if (!scope)
            {
                return;
            }

            for (const auto &ref : scope->references)
            {
                if (ref.name == targetName)
                {
                    // Do not count the declaration site of any symbol in the group
                    bool isDecl = false;
                    for (const auto &sym : group)
                    {
                        if (fileUri == sym.fileUri &&
                            ref.startLine == sym.selectionRange.startLine &&
                            ref.startCharacter == sym.selectionRange.startCharacter)
                        {
                            isDecl = true;
                            break;
                        }
                    }
                    if (isDecl)
                    {
                        continue;
                    }

                    bool isDef = false;
                    for (const auto &def : scope->definitions)
                    {
                        if (def.name == targetName &&
                            def.startLine == ref.startLine &&
                            def.startCharacter == ref.startCharacter)
                        {
                            isDef = true;
                            break;
                        }
                    }
                    if (isDef)
                    {
                        continue;
                    }

                    if (!compatibleClasses.empty())
                    {
                        if (!ref.isMemberAccess)
                        {
                            std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                            if (encClass.empty() || !compatibleClasses.contains(encClass))
                            {
                                continue;
                            }

                            const analysis::LocalDefinition *localShadow = analysis::ResolveInScope(scope, targetName);
                            if (localShadow && (localShadow->kind == analysis::LocalDefinitionKind::Parameter || localShadow->kind == analysis::LocalDefinitionKind::Variable))
                            {
                                continue;
                            }
                        }
                        else
                        {
                            std::string rType;
                            if (fileUri == request.uri && request.tree && !request.sourceCode.empty())
                            {
                                TSNode rootNode = ts_tree_root_node(request.tree);
                                TSPoint pt = { ref.startLine, ref.startCharacter };
                                TSNode refNode = ts_node_descendant_for_point_range(rootNode, pt, pt);
                                if (!ts_node_is_null(refNode))
                                {
                                    TSNode exprParent = ts_node_parent(refNode);
                                    if (!ts_node_is_null(exprParent) && std::string_view(ts_node_type(exprParent)) == "member_expression")
                                    {
                                        TSNode objNode = parser::GetChildByField(exprParent, parser::fields::Object);
                                        if (!ts_node_is_null(objNode))
                                        {
                                            uint32_t oStart = ts_node_start_byte(objNode);
                                            uint32_t oEnd = ts_node_end_byte(objNode);
                                            if (oStart < request.sourceCode.size() && oEnd <= request.sourceCode.size() && oStart < oEnd)
                                            {
                                                std::string oText = request.sourceCode.substr(oStart, oEnd - oStart);
                                                if (oText == "this")
                                                {
                                                    rType = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                                }
                                                else
                                                {
                                                    const analysis::LocalDefinition *oDef = analysis::ResolveInScope(scope, oText);
                                                    if (oDef && !oDef->typeName.empty())
                                                    {
                                                        rType = analysis::CleanBaseType(oDef->typeName);
                                                    }
                                                    else
                                                    {
                                                        auto gSyms = symbolTable.FindSymbols(oText);
                                                        for (const auto &gs : gSyms)
                                                        {
                                                            if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                            {
                                                                rType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                                break;
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (rType.empty())
                            {
                                for (const auto &candRef : scope->references)
                                {
                                    if (candRef.startLine == ref.startLine && candRef.endCharacter <= ref.startCharacter)
                                    {
                                        if (candRef.name == "this")
                                        {
                                            rType = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                                        }
                                        else
                                        {
                                            const analysis::LocalDefinition *oDef = analysis::ResolveInScope(scope, candRef.name);
                                            if (oDef && !oDef->typeName.empty())
                                            {
                                                rType = analysis::CleanBaseType(oDef->typeName);
                                            }
                                            else
                                            {
                                                auto gSyms = symbolTable.FindSymbols(candRef.name);
                                                for (const auto &gs : gSyms)
                                                {
                                                    if (gs.type == analysis::SymbolType::Variable && !gs.GetVariable().typeName.empty())
                                                    {
                                                        rType = analysis::CleanBaseType(gs.GetVariable().typeName);
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (rType.empty() || !compatibleClasses.contains(rType))
                            {
                                continue;
                            }
                        }
                    }
                    else
                    {
                        if (ref.isMemberAccess)
                        {
                            continue;
                        }
                        const analysis::LocalDefinition *localShadow = analysis::ResolveInScope(scope, targetName);
                        if (localShadow && (localShadow->kind == analysis::LocalDefinitionKind::Parameter || localShadow->kind == analysis::LocalDefinitionKind::Variable))
                        {
                            continue;
                        }
                    }

                    const uint64_t pos = (static_cast<uint64_t>(ref.startLine) << 32) | ref.startCharacter;
                    seenRefs.insert({ fileUri, pos });
                }
            }

            for (const auto &child : scope->children)
            {
                CollectReferencesInScope(fileUri, child.get(), targetName, group, compatibleClasses, symbolTable, request, seenRefs);
            }
        }
    }

    std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest &request)
    {
        if (request.sourceCode.empty())
        {
            return std::nullopt;
        }

        // 1. Group symbols declared in request.uri by their declaration range.
        // Multiple host classes synthesizing methods from a mixin produce multiple symbols
        // sharing the same declaration range. Grouping by range ensures strictly 1 CodeLens per range.
        std::vector<RangeKey> rangeOrder;
        ankerl::unordered_dense::map<RangeKey, std::vector<analysis::Symbol>, RangeKeyHash> groups;

        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.fileUri != request.uri)
                {
                    continue;
                }

                const RangeKey key{ sym.startLine, sym.startCharacter, sym.endLine, sym.endCharacter };
                auto [it, inserted] = groups.try_emplace(key, std::vector<analysis::Symbol>{});
                if (inserted)
                {
                    rangeOrder.push_back(key);
                }
                it->second.push_back(sym);
            }
        });

        if (rangeOrder.empty())
        {
            return std::nullopt;
        }

        // Sort ranges so CodeLenses appear in top-down document order
        std::sort(rangeOrder.begin(), rangeOrder.end(), [](const RangeKey &a, const RangeKey &b)
        {
            if (a.startLine != b.startLine)
            {
                return a.startLine < b.startLine;
            }
            return a.startCharacter < b.startCharacter;
        });

        std::vector<lsp::CodeLens> lenses;

        for (const auto &key : rangeOrder)
        {
            auto groupIt = groups.find(key);
            if (groupIt == groups.end() || groupIt->second.empty())
            {
                continue;
            }

            const auto &symGroup = groupIt->second;

            // Pick primary symbol (prefer non-synthesized origin)
            const analysis::Symbol *primarySym = nullptr;
            for (const auto &s : symGroup)
            {
                if (!s.isSynthesized)
                {
                    primarySym = &s;
                    break;
                }
            }
            if (!primarySym)
            {
                primarySym = &symGroup.front();
            }

            const auto &sym = *primarySym;

            if (sym.type == analysis::SymbolType::Function)
            {
                bool isInterfaceMethod = false;
                if (!sym.containerName.empty())
                {
                    auto owners = request.symbolTable.FindSymbolsPtr(sym.containerName);
                    if (owners)
                    {
                        for (const auto &owner : *owners)
                        {
                            if (owner.type == analysis::SymbolType::Interface)
                            {
                                isInterfaceMethod = true;
                                break;
                            }
                        }
                    }
                }

                if (isInterfaceMethod)
                {
                    size_t implCount = 0;
                    request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &candidates)
                    {
                        for (const auto &cand : candidates)
                        {
                            if (cand.type == analysis::SymbolType::Function && cand.name == sym.name &&
                                (cand.fileUri != sym.fileUri || cand.containerName != sym.containerName))
                            {
                                if (!cand.containerName.empty())
                                {
                                    auto candOwners = request.symbolTable.FindSymbolsPtr(cand.containerName);
                                    if (candOwners)
                                    {
                                        for (const auto &cOwner : *candOwners)
                                        {
                                            if (cOwner.type == analysis::SymbolType::Class)
                                            {
                                                for (const auto &b : cOwner.GetClass().bases)
                                                {
                                                    if (analysis::CleanBaseType(b) == sym.containerName)
                                                    {
                                                        if (cand.GetFunction().parameters.size() == sym.GetFunction().parameters.size())
                                                        {
                                                            implCount++;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    });

                    lsp::CodeLens lens;
                    lens.range = lsp::Range{
                        lsp::Position{ key.startLine, key.startCharacter },
                        lsp::Position{ key.endLine, key.endCharacter }
                    };
                    lsp::Command cmd;
                    cmd.title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
                    cmd.command = "";
                    lens.command = std::move(cmd);
                    lenses.push_back(std::move(lens));
                }
                else
                {
                    ankerl::unordered_dense::set<std::string> compatibleClasses;
                    for (const auto &s : symGroup)
                    {
                        if (!s.containerName.empty())
                        {
                            compatibleClasses.insert(s.containerName);
                            auto lastColon = s.containerName.rfind("::");
                            if (lastColon != std::string::npos)
                            {
                                compatibleClasses.insert(s.containerName.substr(lastColon + 2));
                            }
                            auto related = analysis::GetAllRelatedClasses(s.containerName, request.symbolTable);
                            for (const auto &rel : related)
                            {
                                compatibleClasses.insert(rel);
                                auto colon = rel.rfind("::");
                                if (colon != std::string::npos)
                                {
                                    compatibleClasses.insert(rel.substr(colon + 2));
                                }
                            }
                        }
                    }

                    if (!compatibleClasses.empty())
                    {
                        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
                        {
                            for (const auto &cand : symbols)
                            {
                                if (cand.type == analysis::SymbolType::Class && std::holds_alternative<analysis::ClassSignature>(cand.signature))
                                {
                                    const auto &cls = cand.GetClass();
                                    for (const auto &m : cls.includedMixins)
                                    {
                                        if (compatibleClasses.contains(m))
                                        {
                                            compatibleClasses.insert(cand.name);
                                            if (!cand.qualifiedName.empty())
                                            {
                                                compatibleClasses.insert(cand.qualifiedName);
                                            }
                                            auto candRelated = analysis::GetAllRelatedClasses(cand.name, request.symbolTable);
                                            for (const auto &rel : candRelated)
                                            {
                                                compatibleClasses.insert(rel);
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        });
                    }

                    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
                    request.scopeIndex.ForEachScopeTree([&](const std::string &fileUri, const std::shared_ptr<const analysis::Scope> &root)
                    {
                        if (root)
                        {
                            CollectReferencesInScope(fileUri, root.get(), sym.name, symGroup, compatibleClasses, request.symbolTable, request, seenRefs);
                        }
                    });

                    const size_t refCount = seenRefs.size();
                    lsp::CodeLens lens;
                    lens.range = lsp::Range{
                        lsp::Position{ key.startLine, key.startCharacter },
                        lsp::Position{ key.endLine, key.endCharacter }
                    };
                    lsp::Command cmd;
                    cmd.title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
                    cmd.command = "";
                    lens.command = std::move(cmd);
                    lenses.push_back(std::move(lens));
                }
            }
            else if (sym.type == analysis::SymbolType::Interface)
            {
                size_t implCount = 0;
                request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &candidates)
                {
                    for (const auto &cand : candidates)
                    {
                        if (cand.type == analysis::SymbolType::Class)
                        {
                            for (const auto &b : cand.GetClass().bases)
                            {
                                if (analysis::CleanBaseType(b) == sym.name)
                                {
                                    implCount++;
                                    break;
                                }
                            }
                        }
                    }
                });

                lsp::CodeLens lens;
                lens.range = lsp::Range{
                    lsp::Position{ key.startLine, key.startCharacter },
                    lsp::Position{ key.endLine, key.endCharacter }
                };
                lsp::Command cmd;
                cmd.title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
                cmd.command = "";
                lens.command = std::move(cmd);
                lenses.push_back(std::move(lens));
            }
            else if (sym.type == analysis::SymbolType::Class)
            {
                ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
                request.scopeIndex.ForEachScopeTree([&](const std::string &fileUri, const std::shared_ptr<const analysis::Scope> &root)
                {
                    if (root)
                    {
                        CollectReferencesInScope(fileUri, root.get(), sym.name, symGroup, {}, request.symbolTable, request, seenRefs);
                    }
                });

                const size_t refCount = seenRefs.size();
                lsp::CodeLens lens;
                lens.range = lsp::Range{
                    lsp::Position{ key.startLine, key.startCharacter },
                    lsp::Position{ key.endLine, key.endCharacter }
                };
                lsp::Command cmd;
                cmd.title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
                cmd.command = "";
                lens.command = std::move(cmd);
                lenses.push_back(std::move(lens));
            }
        }

        if (lenses.empty())
        {
            return std::nullopt;
        }

        return lenses;
    }

    std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest &request)
    {
        return request.codeLens;
    }
}
