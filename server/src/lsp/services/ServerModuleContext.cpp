#include "analysis/index/RuleIndex.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace angel_lsp
{
static bool SymbolDeclaresShared(const angel_lsp::analysis::Symbol& symbol)
{
    if (symbol.type == angel_lsp::analysis::SymbolType::Class)
    {
        const auto& c = symbol.GetClass();
        return c.hasBraces && c.modifiers.isShared && !c.modifiers.isExternal;
    }
    if (symbol.type == angel_lsp::analysis::SymbolType::Function)
    {
        const auto& f = symbol.GetFunction();
        return f.hasBody && f.modifiers.isShared && !f.modifiers.isExternal;
    }
    return false;
}

void Server::SyncModuleIndexSymbols()
{
    std::vector<ModuleIndex::ExportedSymbol> exported;
    for (const auto& view : m_modules)
    {
        exported.push_back(ModuleIndex::ExportedSymbol{view.name, view.folderPath, "", 0, 0});
    }
    if (const auto ruleIndex = m_symbolTable.GetRuleIndex())
    {
        for (const auto& [key, _] : ruleIndex->sharedDeclarationFilesByKey)
        {
            auto symsPtr = m_symbolTable.FindSymbolsPtr(key);
            if (!symsPtr)
            {
                continue;
            }
            for (const auto& sym : *symsPtr)
            {
                if (SymbolDeclaresShared(sym))
                {
                    exported.push_back(ModuleIndex::ExportedSymbol{sym.name, sym.containerName, sym.fileUri,
                                                                   sym.startLine, sym.startCharacter});
                }
            }
        }
    }
    GetModuleIndex().SetExportedSymbols(std::move(exported));
}

void Server::CollectSharedSymbolsElsewhere(
    const ModuleView& owning,
    ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>>& outShared) const
{
    const auto ruleIndex = m_symbolTable.GetRuleIndex();
    if (!ruleIndex)
    {
        return;
    }
    for (const auto& [name, fileUris] : ruleIndex->sharedDeclarationFilesByKey)
    {
        for (const auto& fileUri : fileUris)
        {
            const std::string declaringPath = CanonicalPathFromUri(fileUri);
            if (!declaringPath.empty() && !owning.memberPaths.contains(declaringPath))
            {
                outShared.insert(name);
                break;
            }
        }
    }
}

std::optional<angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext>
Server::ModuleContextFor(std::string_view uriStr) const
{
    if (m_modules.empty())
    {
        return std::nullopt;
    }

    const std::string path = CanonicalPathFromUri(uriStr);
    if (path.empty())
    {
        return std::nullopt;
    }

    angel_lsp::analysis::SemanticAnalysisRequest::ModuleContext context;
    context.moduleNames.reserve(m_modules.size());
    for (const auto& view : m_modules)
    {
        context.moduleNames.push_back(view.name);
    }

    const ModuleClaim claim = ClaimFor(path);
    const ModuleView* owning = claim.owner;
    context.alsoClaimedBy = claim.alsoClaimedBy;

    if (owning == nullptr)
    {
        return context;
    }

    context.name = owning->name;
    CollectSharedSymbolsElsewhere(*owning, context.sharedElsewhere);

    return context;
}

static void AppendConfiguredPathAndParent(const Server& server, std::string_view raw, std::vector<std::string>& roots)
{
    if (raw.empty())
    {
        return;
    }
    const std::string resolved = server.ResolveConfiguredPath(raw);
    if (resolved.empty())
    {
        return;
    }
    roots.push_back(resolved);
    const std::filesystem::path p(resolved);
    if (p.has_parent_path())
    {
        roots.push_back(p.parent_path().string());
    }
}

static void CollectConfiguredAllowedRoots(const Server& server, const angel_lsp::config::ServerConfig& config,
                                          std::vector<std::string>& roots)
{
    for (const auto& predefined : config.predefinedFiles)
    {
        AppendConfiguredPathAndParent(server, predefined, roots);
    }

    if (!config.activePredefined.empty() && config.activePredefined != "all")
    {
        AppendConfiguredPathAndParent(server, config.activePredefined, roots);
    }

    for (const auto& definition : config.modules)
    {
        AppendConfiguredPathAndParent(server, definition.entry, roots);
        AppendConfiguredPathAndParent(server, definition.folder, roots);
    }

    AppendConfiguredPathAndParent(server, config.moduleEntryPoint, roots);

    for (const auto& forceFile : config.forceIncludeFiles)
    {
        AppendConfiguredPathAndParent(server, forceFile, roots);
    }
}

std::vector<std::string> Server::IncludeAllowedRoots() const
{
    std::vector<std::string> roots;

    for (const auto& workspaceRoot : WorkspaceRoots())
    {
        std::string path = angel_lsp::utils::UriToPath(workspaceRoot);
        if (!path.empty())
        {
            roots.push_back(path);
            const std::string norm = angel_lsp::utils::IncludeResolver::NormalizePath(path);
            if (!norm.empty() && norm != path)
            {
                roots.push_back(norm);
            }
        }
    }

    const auto searchDirectories = SearchDirectories();
    roots.insert(roots.end(), searchDirectories->begin(), searchDirectories->end());

    CollectConfiguredAllowedRoots(*this, m_config, roots);

    if (roots.empty())
    {
        for (const auto& uri : GetDocumentStore().GetOpenUris())
        {
            const std::string p = CanonicalPathFromUri(uri);
            if (!p.empty())
            {
                std::filesystem::path fp(p);
                if (fp.has_parent_path())
                {
                    roots.push_back(fp.parent_path().string());
                }
            }
        }
    }

    return roots;
}
} // namespace angel_lsp
