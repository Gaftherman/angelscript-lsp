#include "features/code_lens/CodeLensHandler.h"
#include "analysis/SemanticHelpers.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::features
{
    namespace
    {
        /**
         * @brief Recursively traverses lexical scopes to count references to a specific target symbol.
         * @param scope Current scope to check.
         * @param targetSym Target symbol being counted.
         * @param count Running count of references.
         */
        void CountReferencesInScope(const analysis::Scope *scope, const analysis::Symbol &targetSym, size_t &count)
        {
            if (!scope)
            {
                return;
            }

            for (const auto &ref : scope->references)
            {
                if (ref.name == targetSym.name)
                {
                    // Do not count the declaration site itself
                    if (ref.startLine == targetSym.selectionRange.startLine &&
                        ref.startCharacter == targetSym.selectionRange.startCharacter)
                    {
                        continue;
                    }

                    bool isDef = false;
                    for (const auto &def : scope->definitions)
                    {
                        if (def.name == targetSym.name &&
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

                    count++;
                }
            }

            for (const auto &child : scope->children)
            {
                CountReferencesInScope(child.get(), targetSym, count);
            }
        }
    }

    std::optional<std::vector<lsp::CodeLens>> GetCodeLenses(const CodeLensRequest &request)
    {
        if (request.sourceCode.empty())
        {
            return std::nullopt;
        }

        std::vector<lsp::CodeLens> lenses;

        request.symbolTable.ForEachSymbol([&](const std::string &, const std::vector<analysis::Symbol> &symbols)
        {
            for (const auto &sym : symbols)
            {
                if (sym.fileUri != request.uri)
                {
                    continue;
                }

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
                            lsp::Position{ sym.startLine, sym.startCharacter },
                            lsp::Position{ sym.endLine, sym.endCharacter }
                        };
                        lsp::Command cmd;
                        cmd.title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
                        cmd.command = "";
                        lens.command = std::move(cmd);
                        lenses.push_back(std::move(lens));
                    }
                    else
                    {
                        size_t refCount = 0;
                        request.scopeIndex.ForEachScopeTree([&](const std::string &, const std::shared_ptr<const analysis::Scope> &root)
                        {
                            if (root)
                            {
                                CountReferencesInScope(root.get(), sym, refCount);
                            }
                        });

                        lsp::CodeLens lens;
                        lens.range = lsp::Range{
                            lsp::Position{ sym.startLine, sym.startCharacter },
                            lsp::Position{ sym.endLine, sym.endCharacter }
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
                        lsp::Position{ sym.startLine, sym.startCharacter },
                        lsp::Position{ sym.endLine, sym.endCharacter }
                    };
                    lsp::Command cmd;
                    cmd.title = std::to_string(implCount) + (implCount == 1 ? " implementation" : " implementations");
                    cmd.command = "";
                    lens.command = std::move(cmd);
                    lenses.push_back(std::move(lens));
                }
                else if (sym.type == analysis::SymbolType::Class)
                {
                    size_t refCount = 0;
                    request.scopeIndex.ForEachScopeTree([&](const std::string &, const std::shared_ptr<const analysis::Scope> &root)
                    {
                        if (root)
                        {
                            CountReferencesInScope(root.get(), sym, refCount);
                        }
                    });

                    lsp::CodeLens lens;
                    lens.range = lsp::Range{
                        lsp::Position{ sym.startLine, sym.startCharacter },
                        lsp::Position{ sym.endLine, sym.endCharacter }
                    };
                    lsp::Command cmd;
                    cmd.title = std::to_string(refCount) + (refCount == 1 ? " reference" : " references");
                    cmd.command = "";
                    lens.command = std::move(cmd);
                    lenses.push_back(std::move(lens));
                }
            }
        });

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
