#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SemanticHelpers.h"
#include "parser/GrammarNames.h"
#include "parser/AngelScriptParser.h"
#include "utils/Utils.h"
#include "utils/LspLogger.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>
#include <ankerl/unordered_dense.h>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp::features
{
    namespace
    {
        TSNode GetChildByField(TSNode node, const char *fieldName)
        {
            return ts_node_child_by_field_name(node, fieldName, static_cast<uint32_t>(strlen(fieldName)));
        }

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
                                     const ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>> &allDeclRanges,
                                     analysis::AccessModifier targetAccess,
                                     bool isFunction,
                                     size_t minArgs,
                                     size_t maxArgs,
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
                    if (allDeclRanges.contains({ fileUri, ref.startLine, ref.startCharacter }))
                    {
                        continue;
                    }

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


                    if (isFunction)
                    {
                        if (ref.isCall)
                        {
                            if (ref.argumentCount < minArgs || ref.argumentCount > maxArgs)
                            {
                                continue;
                            }
                        }
                    }

                    if (!compatibleClasses.empty())
                    {
                        if (targetAccess == analysis::AccessModifier::Private ||
                            targetAccess == analysis::AccessModifier::Protected)
                        {
                            std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                            if (encClass.empty() || !compatibleClasses.contains(encClass))
                            {
                                continue;
                            }
                        }

                        if (!ref.isMemberAccess)
                        {
                            std::string encClass = GetEnclosingClassName(symbolTable, fileUri, ref.startLine);
                            if (encClass.empty() || !compatibleClasses.contains(encClass))
                            {
                                continue;
                            }

                            bool inTargetHierarchy = false;
                            std::string cleanEnc = analysis::CleanBaseType(encClass);
                            for (const auto &s : group)
                            {
                                if (s.containerName.empty())
                                {
                                    continue;
                                }
                                std::string cleanDecl = analysis::CleanBaseType(s.containerName);
                                if (cleanEnc == cleanDecl)
                                {
                                    inTargetHierarchy = true;
                                    break;
                                }
                                auto hierarchy = analysis::GetInheritedTypeHierarchy(cleanEnc, symbolTable);
                                for (const auto &ancestor : hierarchy)
                                {
                                    if (analysis::CleanBaseType(ancestor) == cleanDecl)
                                    {
                                        inTargetHierarchy = true;
                                        break;
                                    }
                                }
                                if (inTargetHierarchy)
                                {
                                    break;
                                }
                            }
                            if (!inTargetHierarchy)
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
                CollectReferencesInScope(fileUri, child.get(), targetName, group, compatibleClasses, allDeclRanges, targetAccess, isFunction, minArgs, maxArgs, symbolTable, request, seenRefs);
            }

        }
    }

    std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest &request)
    {
        if (request.logger && request.logger->IsDebugEnabled())
        {
            request.logger->LogDebug(fmt::format("[CodeLens] Computing code lenses for URI: {}", request.uri));
        }

        if (request.uri.starts_with("angelscript-virtual:") || request.uri.starts_with("angelscript-virtual://"))
        {
            std::string_view s = request.uri;
            static constexpr std::string_view kVirtualSchemeFull = "angelscript-virtual://";
            static constexpr std::string_view kVirtualSchemeShort = "angelscript-virtual:";

            if (s.starts_with(kVirtualSchemeFull))
            {
                s.remove_prefix(kVirtualSchemeFull.size());
            }
            else if (s.starts_with(kVirtualSchemeShort))
            {
                s.remove_prefix(kVirtualSchemeShort.size());
                while (!s.empty() && s.front() == '/')
                {
                    s.remove_prefix(1);
                }
            }

            std::string mixinName;
            auto slashPos = s.find('/');
            if (slashPos != std::string_view::npos)
            {
                std::string_view mixinPart = s.substr(slashPos + 1);
                if (mixinPart.ends_with(".as"))
                {
                    mixinPart.remove_suffix(3);
                }
                mixinName = utils::UrlDecode(mixinPart);
            }
            else
            {
                std::string_view mixinPart = s;
                if (mixinPart.ends_with(".as"))
                {
                    mixinPart.remove_suffix(3);
                }
                mixinName = utils::UrlDecode(mixinPart);
            }

            const analysis::Symbol *mixinSym = nullptr;
            auto candidates = request.symbolTable.FindSymbols(mixinName);
            for (const auto &c : candidates)
            {
                if (c.type == analysis::SymbolType::Class && c.GetClass().modifiers.isMixin)
                {
                    mixinSym = &c;
                    break;
                }
            }
            if (!mixinSym)
            {
                std::string shortName = mixinName;
                auto lastScope = shortName.rfind("::");
                if (lastScope != std::string::npos)
                {
                    shortName = shortName.substr(lastScope + 2);
                }
                auto shortCands = request.symbolTable.FindTypeSymbolsByShortName(shortName);
                for (const auto &c : shortCands)
                {
                    if (c.type == analysis::SymbolType::Class && c.GetClass().modifiers.isMixin)
                    {
                        mixinSym = &c;
                        break;
                    }
                }
            }

            if (mixinSym && !mixinSym->fileUri.empty())
            {
                std::string physicalPath = utils::UriToPath(mixinSym->fileUri);
                std::string filename = std::filesystem::path(physicalPath).filename().string();
                if (filename.empty())
                {
                    filename = mixinSym->fileUri;
                }

                lsp::CodeLens lens;
                lens.range = lsp::Range{
                    lsp::Position{ 0, 0 },
                    lsp::Position{ 0, 0 }
                };
                lsp::Command cmd;
                cmd.title = fmt::format("Jump to physical source in {}", filename);
                cmd.command = "angelscript.openPhysicalSource";

                lsp::json::Array args;
                lsp::json::Object argObj;
                argObj["fileUri"] = lsp::json::Value(std::string(mixinSym->fileUri));
                argObj["line"] = static_cast<lsp::json::Integer>(mixinSym->startLine);
                argObj["character"] = static_cast<lsp::json::Integer>(mixinSym->startCharacter);
                args.push_back(lsp::json::Value(std::move(argObj)));
                cmd.arguments = std::move(args);

                lens.command = std::move(cmd);
                return std::vector<lsp::CodeLens>{ std::move(lens) };
            }
            return std::nullopt;
        }

        if (request.sourceCode.empty())
        {
            return std::nullopt;
        }

        // 1. Group symbols declared in request.uri by their declaration range.
        // Multiple host classes synthesizing methods from a mixin produce multiple symbols
        // sharing the same declaration range. Grouping by range ensures strictly 1 CodeLens per range.
        std::vector<RangeKey> rangeOrder;
        ankerl::unordered_dense::map<RangeKey, std::vector<analysis::Symbol>, RangeKeyHash> groups;

        request.symbolTable.ForEachSymbolInFile(request.uri, [&](const std::string &, const std::vector<analysis::Symbol> &symbols)
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
                    analysis::AccessModifier targetAccess = analysis::AccessModifier::Public;
                    bool isFunction = false;
                    size_t minArgs = 0;
                    size_t maxArgs = 0;
                    if (std::holds_alternative<analysis::FunctionSignature>(sym.signature))
                    {
                        targetAccess = sym.GetFunction().modifiers.access;
                        isFunction = true;
                        const auto &fn = sym.GetFunction();
                        maxArgs = fn.parameters.size();
                        for (const auto &p : fn.parameters)
                        {
                            if (p.defaultValue.empty())
                            {
                                minArgs++;
                            }
                        }
                    }
                    else if (std::holds_alternative<analysis::VariableSignature>(sym.signature))
                    {
                        targetAccess = sym.GetVariable().modifiers.access;
                    }

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
                            auto comp = analysis::GetCompatibleMemberClasses(s.containerName, sym.name, targetAccess, request.symbolTable);
                            for (const auto &c : comp)
                            {
                                compatibleClasses.insert(c);
                            }
                        }
                    }

                    if (!compatibleClasses.empty() && targetAccess != analysis::AccessModifier::Private)
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
                                            auto candDerived = analysis::GetDerivedClasses(cand.name, request.symbolTable);
                                            for (const auto &rel : candDerived)
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

                    ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
                    auto addDecls = [&](const std::string &qName)
                    {
                        auto syms = request.symbolTable.FindSymbols(qName);
                        for (const auto &s : syms)
                        {
                            if (s.type != analysis::SymbolType::CallReference)
                            {
                                uint32_t sL = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startLine : s.startLine;
                                uint32_t sC = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startCharacter : s.startCharacter;
                                allDeclRanges.insert({ s.fileUri, sL, sC });
                            }
                        }
                    };

                    addDecls(sym.name);
                    for (const auto &c : compatibleClasses)
                    {
                        addDecls(c + "::" + sym.name);
                    }
                    for (const auto &s : symGroup)
                    {
                        uint32_t sL = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startLine : s.startLine;
                        uint32_t sC = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startCharacter : s.startCharacter;
                        allDeclRanges.insert({ s.fileUri, sL, sC });
                    }

                    ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
                    request.scopeIndex.ForEachScopeTree([&](const std::string &fileUri, const std::shared_ptr<const analysis::Scope> &root)
                    {
                        if (root)
                        {
                            CollectReferencesInScope(fileUri, root.get(), sym.name, symGroup, compatibleClasses, allDeclRanges, targetAccess, isFunction, minArgs, maxArgs, request.symbolTable, request, seenRefs);
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
                ankerl::unordered_dense::set<std::tuple<std::string, uint32_t, uint32_t>> allDeclRanges;
                for (const auto &s : symGroup)
                {
                    uint32_t sL = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startLine : s.startLine;
                    uint32_t sC = (s.selectionRange.endLine > 0 || s.selectionRange.endCharacter > 0) ? s.selectionRange.startCharacter : s.startCharacter;
                    allDeclRanges.insert({ s.fileUri, sL, sC });
                }

                ankerl::unordered_dense::set<std::pair<std::string, uint64_t>> seenRefs;
                request.scopeIndex.ForEachScopeTree([&](const std::string &fileUri, const std::shared_ptr<const analysis::Scope> &root)
                {
                    if (root)
                    {
                        CollectReferencesInScope(fileUri, root.get(), sym.name, symGroup, {}, allDeclRanges, analysis::AccessModifier::Public, false, 0, 0, request.symbolTable, request, seenRefs);
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

                if (std::holds_alternative<analysis::ClassSignature>(sym.signature))
                {
                    const auto &sig = sym.GetClass();
                    if (!sig.modifiers.isMixin)
                    {
                        std::vector<std::string> includedMixins = sig.includedMixins;
                        for (const auto &b : sig.bases)
                        {
                            std::string clean = analysis::CleanBaseType(b);
                            if (!clean.empty() && analysis::IsMixinClass(clean, request.symbolTable))
                            {
                                if (std::find(includedMixins.begin(), includedMixins.end(), clean) == includedMixins.end())
                                {
                                    includedMixins.push_back(clean);
                                }
                            }
                        }

                        for (const auto &mixinName : includedMixins)
                        {
                            uint32_t incStartLine = sym.startLine;
                            uint32_t incStartChar = sym.startCharacter;
                            uint32_t incEndLine = sym.startLine;
                            uint32_t incEndChar = sym.endCharacter;

                            if (request.tree && !request.sourceCode.empty())
                            {
                                TSNode hostRoot = ts_tree_root_node(request.tree);
                                TSPoint hostPt = { sym.startLine, sym.startCharacter };
                                TSNode hostNode = ts_node_descendant_for_point_range(hostRoot, hostPt, hostPt);
                                while (!ts_node_is_null(hostNode) && std::string_view(ts_node_type(hostNode)) != "class_declaration")
                                {
                                    hostNode = ts_node_parent(hostNode);
                                }

                                if (!ts_node_is_null(hostNode))
                                {
                                    bool foundInc = false;
                                    uint32_t childCount = ts_node_child_count(hostNode);
                                    for (uint32_t c = 0; c < childCount; ++c)
                                    {
                                        TSNode child = ts_node_child(hostNode, c);
                                        if (std::string_view(ts_node_type(child)) == "base_class_list")
                                        {
                                            uint32_t bCount = ts_node_named_child_count(child);
                                            for (uint32_t b = 0; b < bCount; ++b)
                                            {
                                                TSNode baseChild = ts_node_named_child(child, b);
                                                std::string bText = std::string(parser::AngelScriptParser::GetNodeText(baseChild, request.sourceCode));
                                                if (analysis::CleanBaseType(bText) == mixinName || bText == mixinName || bText.ends_with("::" + mixinName))
                                                {
                                                    TSPoint sp = ts_node_start_point(baseChild);
                                                    TSPoint ep = ts_node_end_point(baseChild);
                                                    incStartLine = sp.row;
                                                    incStartChar = sp.column;
                                                    incEndLine = ep.row;
                                                    incEndChar = ep.column;
                                                    foundInc = true;
                                                    break;
                                                }
                                            }
                                        }
                                        if (foundInc) break;
                                    }

                                    if (!foundInc)
                                    {
                                        TSNode bodyNode = GetChildByField(hostNode, "body");
                                        if (!ts_node_is_null(bodyNode))
                                        {
                                            uint32_t mCount = ts_node_child_count(bodyNode);
                                            for (uint32_t m = 0; m < mCount; ++m)
                                            {
                                                TSNode mNode = ts_node_child(bodyNode, m);
                                                std::string mText = std::string(parser::AngelScriptParser::GetNodeText(mNode, request.sourceCode));
                                                if (mText.find(mixinName) != std::string::npos)
                                                {
                                                    TSPoint sp = ts_node_start_point(mNode);
                                                    TSPoint ep = ts_node_end_point(mNode);
                                                    incStartLine = sp.row;
                                                    incStartChar = sp.column;
                                                    incEndLine = ep.row;
                                                    incEndChar = ep.column;
                                                    foundInc = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            lsp::CodeLens mixinLens;
                            mixinLens.range = lsp::Range{
                                lsp::Position{ incStartLine, incStartChar },
                                lsp::Position{ incEndLine, incEndChar }
                            };
                            lsp::Command mixinCmd;
                            mixinCmd.title = fmt::format("View Mixin Expansion: {}", mixinName);
                            mixinCmd.command = "angelscript.viewMixinExpansion";

                            lsp::json::Array args;
                            lsp::json::Object argObj;
                            argObj["hostClass"] = lsp::json::Value(std::string(sym.qualifiedName.empty() ? sym.name : sym.qualifiedName));
                            argObj["mixinName"] = lsp::json::Value(std::string(mixinName));
                            argObj["hostUri"] = lsp::json::Value(std::string(request.uri));
                            argObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(incStartLine));
                            argObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(incStartChar));

                            if (auto symsPtr = request.symbolTable.FindSymbolsPtr(mixinName))
                            {
                                for (const auto &s : *symsPtr)
                                {
                                    if (s.type == analysis::SymbolType::Class && s.GetClass().modifiers.isMixin)
                                    {
                                        argObj["targetUri"] = lsp::json::Value(std::string(s.fileUri));
                                        argObj["targetLine"] = lsp::json::Value(static_cast<lsp::json::Integer>(s.startLine));
                                        argObj["targetCharacter"] = lsp::json::Value(static_cast<lsp::json::Integer>(s.startCharacter));
                                        break;
                                    }
                                }
                            }

                            args.push_back(lsp::json::Value(std::move(argObj)));
                            mixinCmd.arguments = std::move(args);

                            mixinLens.command = std::move(mixinCmd);
                            lenses.push_back(std::move(mixinLens));
                        }
                    }
                }
            }
        }

        if (lenses.empty())
        {
            return std::nullopt;
        }

        if (request.logger && request.logger->IsTraceEnabled())
        {
            request.logger->LogTrace(fmt::format("[CodeLens] Generated {} code lenses for URI: {}", lenses.size(), request.uri));
        }

        return lenses;
    }

    std::optional<lsp::CodeLens> ResolveCodeLens(const CodeLensResolveRequest &request)
    {
        if (request.logger && request.logger->IsTraceEnabled())
        {
            request.logger->LogTrace("[CodeLens] Resolving code lens");
        }
        return request.codeLens;
    }
}
