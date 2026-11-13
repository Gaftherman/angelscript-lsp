#include "lsp/Server.h"
#include "lsp/handlers/WorkspaceConfigHelpers.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

static bool BraceStyleIsKR(std::string_view name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered == "kr" || lowered == "k&r" || lowered == "kandr" || lowered == "onetbs";
}

static void UpdateFormatConfiguration(Server& server, const lsp::LSPObject& section)
{
    const lsp::LSPObject* formatObj = nullptr;
    if (const auto* formatVal = section.find("format"); formatVal && formatVal->isObject())
    {
        formatObj = &formatVal->object();
    }
    if (auto styleVal = FindSectionString(section, formatObj, "braceStyle", "format"); styleVal.has_value())
    {
        const bool wantsKR = BraceStyleIsKR(*styleVal);
        const bool prevKR = server.GetSessionCoordinator().GetFormatBraceStyleKR();
        if (wantsKR != prevKR)
        {
            server.GetSessionCoordinator().SetFormatBraceStyleKR(wantsKR);
            server.LogInfo(fmt::format("Format brace style changed to '{}'", *styleVal));
        }
    }
    if (auto spacesVal = FindSectionBool(section, formatObj, "spacesInsideParentheses", "format");
        spacesVal.has_value())
    {
        server.GetSessionCoordinator().SetFormatSpacesInsideParentheses(*spacesVal);
    }
    if (auto keepEmpty = FindSectionBool(section, formatObj, "keepEmptyBlocksOnSingleLine", "format");
        keepEmpty.has_value())
    {
        server.GetSessionCoordinator().SetFormatKeepEmptyBlocksOnSingleLine(*keepEmpty);
    }
    if (auto fosVal = FindSectionBool(section, formatObj, "onSave", "format"); fosVal.has_value())
    {
        server.GetMutableConfig().format.formatOnSave = *fosVal;
    }
    if (auto alignVal = FindSectionString(section, formatObj, "pointerAlignment", "format"); alignVal.has_value())
    {
        analysis::formatting::PointerAlignment align = analysis::formatting::PointerAlignment::Left;
        if (*alignVal == "right" || *alignVal == "Right")
        {
            align = analysis::formatting::PointerAlignment::Right;
        }
        else if (*alignVal == "middle" || *alignVal == "Middle")
        {
            align = analysis::formatting::PointerAlignment::Middle;
        }
        server.GetSessionCoordinator().SetFormatPointerAlignment(align);
    }
}

static std::vector<config::ServerConfig::ModuleDefinition> ParseModuleDefinitions(const lsp::LSPArray& items)
{
    std::vector<config::ServerConfig::ModuleDefinition> parsed;
    for (const auto& item : items)
    {
        if (!item.isObject())
        {
            continue;
        }

        config::ServerConfig::ModuleDefinition definition;
        if (const auto* nameVal = item.object().find("name"); nameVal && nameVal->isString())
            definition.name = nameVal->string();
        if (const auto* entryVal = item.object().find("entry"); entryVal && entryVal->isString())
            definition.entry = entryVal->string();
        if (const auto* folderVal = item.object().find("folder"); folderVal && folderVal->isString())
            definition.folder = folderVal->string();

        parsed.push_back(std::move(definition));
    }
    return parsed;
}

static bool UpdateModulesConfiguration(Server& server, const lsp::LSPObject& section)
{
    auto& config = server.GetMutableConfig();
    bool entryPointChanged = false;
    if (const auto* entryVal = section.find("moduleEntryPoint"); entryVal && entryVal->isString())
    {
        if (entryVal->string() != config.moduleEntryPoint)
        {
            config.moduleEntryPoint = entryVal->string();
            entryPointChanged = true;
            server.LogInfo(fmt::format("Module entry point changed to '{}'", config.moduleEntryPoint));
        }
    }

    const auto* modulesVal = section.find("modules");
    if (!modulesVal || !modulesVal->isArray())
    {
        return entryPointChanged;
    }

    auto parsed = ParseModuleDefinitions(modulesVal->array());

    const bool changed =
        parsed.size() != config.modules.size() ||
        !std::equal(parsed.begin(), parsed.end(), config.modules.begin(),
                    [](const config::ServerConfig::ModuleDefinition& a, const config::ServerConfig::ModuleDefinition& b)
                    { return a.name == b.name && a.entry == b.entry && a.folder == b.folder; });

    if (changed)
    {
        config.modules = std::move(parsed);
        server.LogInfo(fmt::format("Modules changed ({} configured); rescanning", config.modules.size()));
        server.WithdrawStaleModuleDiagnostics();
        return true;
    }
    if (entryPointChanged)
    {
        server.WithdrawStaleModuleDiagnostics();
    }
    return entryPointChanged;
}

static bool UpdateIncludeConfiguration(Server& server, const lsp::LSPObject& section)
{
    auto& config = server.GetMutableConfig();
    bool changed = false;
    if (const auto* includeVal = section.find("include"); includeVal && includeVal->isObject())
    {
        if (const auto* implicitVal = includeVal->object().find("implicitExtension");
            implicitVal && implicitVal->isBoolean())
        {
            if (implicitVal->boolean() != config.implicitIncludeExtension)
            {
                config.implicitIncludeExtension = implicitVal->boolean();
                server.LogInfo(fmt::format("Implicit include extension {}; rebuilding the include graph",
                                           config.implicitIncludeExtension ? "on" : "off"));
                changed = true;
            }
        }
    }

    if (const auto* forceVal = section.find("forceIncludeFiles"); forceVal && forceVal->isArray())
    {
        std::vector<std::string> updated;
        for (const auto& item : forceVal->array())
        {
            if (item.isString() && !item.string().empty())
                updated.push_back(item.string());
        }
        if (updated != config.forceIncludeFiles)
        {
            config.forceIncludeFiles = std::move(updated);
            server.LogInfo(fmt::format("Force include files changed ({} entries)", config.forceIncludeFiles.size()));
            changed = true;
        }
    }
    return changed;
}

static bool UpdateActiveStubFiles(angel_lsp::config::ServerConfig& config, const lsp::LSPObject& section)
{
    const auto* stubsVal = section.find("stubs");
    if (!stubsVal || !stubsVal->isObject())
    {
        return false;
    }
    const auto* activeFilesVal = stubsVal->object().find("activeFiles");
    if (!activeFilesVal || !activeFilesVal->isArray())
    {
        return false;
    }
    std::vector<std::string> activeFiles;
    for (const auto& item : activeFilesVal->array())
    {
        if (item.isString() && !item.string().empty())
        {
            activeFiles.push_back(item.string());
        }
    }
    if (activeFiles != config.activeStubFiles)
    {
        config.activeStubFiles = std::move(activeFiles);
        return true;
    }
    return false;
}

static bool UpdatePredefinedConfiguration(Server& server, const lsp::LSPObject& section)
{
    auto& config = server.GetMutableConfig();
    bool shouldRescan = false;

    if (const auto* predefinedVal = section.find("predefined"); predefinedVal && predefinedVal->isObject())
    {
        if (const auto* activeVal = predefinedVal->object().find("active"); activeVal && activeVal->isString())
        {
            if (activeVal->string() != config.activePredefined)
            {
                config.activePredefined = activeVal->string();
                server.LogInfo(
                    fmt::format("Active predefined stub changed to '{}'; rescanning",
                                config.activePredefined.empty() ? std::string("<all>") : config.activePredefined));
                shouldRescan = true;
            }
        }
    }

    if (UpdateActiveStubFiles(config, section))
    {
        server.LogInfo(fmt::format("Active stub files changed ({} active); rescanning", config.activeStubFiles.size()));
        shouldRescan = true;
    }

    if (const auto* rulesFileVal = section.find("rulesFile"); rulesFileVal && rulesFileVal->isString())
    {
        if (rulesFileVal->string() != config.rulesFile)
        {
            config.rulesFile = rulesFileVal->string();
            server.LogInfo(fmt::format("Rules file changed to '{}'; reloading engine rules", config.rulesFile));
            shouldRescan = true;
        }
    }

    return shouldRescan;
}

static bool UpdateSearchDirectoriesConfiguration(Server& server, const lsp::LSPObject& section)
{
    const auto* directories = section.find("searchDirectories");
    if (!directories || !directories->isArray())
    {
        return false;
    }

    std::vector<std::string> updated;
    for (const auto& entry : directories->array())
    {
        if (entry.isString() && !entry.string().empty())
        {
            updated.push_back(entry.string());
        }
    }

    if (updated != *server.SearchDirectories())
    {
        const size_t count = updated.size();
        server.GetWorkspaceStore().SetSearchDirectories(std::move(updated));
        server.LogInfo(fmt::format("Search directories changed ({} entries); rebuilding the include graph", count));
        return true;
    }
    return false;
}

void Server::HandleNotificationsWorkspace_DidChangeConfiguration(
    lsp::notifications::Workspace_DidChangeConfiguration::Params&& params)
{
    if (!params.settings.isObject())
    {
        return;
    }

    const lsp::LSPObject* section = &params.settings.object();
    if (const auto* nested = section->find("angelscript"); nested && nested->isObject())
    {
        section = &nested->object();
    }

    UpdateFormatConfiguration(*this, *section);
    bool shouldReanalyse;
    {
        std::lock_guard lock(m_lifecycleMutex);
        shouldReanalyse = UpdateEngineConfiguration(*this, *section);
        shouldReanalyse = UpdateDiagnosticsConfiguration(*this, *section) || shouldReanalyse;
        if (shouldReanalyse)
        {
            m_sessionCoordinator->IncrementConfigRevision();
        }
    }
    if (shouldReanalyse)
    {
        ReanalyseOpenDocuments();
    }
    UpdateFeatureConfiguration(*this, *section);

    bool shouldRescan = false;
    shouldRescan = UpdateModulesConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdateIncludeConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdatePredefinedConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdateSearchDirectoriesConfiguration(*this, *section) || shouldRescan;

    if (shouldRescan)
    {
        RestartWorkspaceScan();
    }
}

} // namespace angel_lsp
