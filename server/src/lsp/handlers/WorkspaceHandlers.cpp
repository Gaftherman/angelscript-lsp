#include "features/formatting/PredefinedStubFormatter.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "lsp/PositionCodec.h"
#include "lsp/Server.h"
#include "utils/Utils.h"
#include <cctype>
#include <fstream>

namespace angel_lsp
{
namespace
{
bool BraceStyleIsKR(std::string_view name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lowered == "kr" || lowered == "k&r" || lowered == "kandr" || lowered == "onetbs";
}

std::optional<bool> FindSectionBool(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                    std::string_view name)
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isBoolean())
        {
            return val->boolean();
        }
    }
    const std::string dotKey = "engine." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    return std::nullopt;
}

std::optional<int> FindSectionInt(const lsp::LSPObject& section, const lsp::LSPObject* engineObj, std::string_view name)
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isNumber())
        {
            return static_cast<int>(val->number());
        }
    }
    const std::string dotKey = "engine." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}
} // namespace

lsp::requests::Workspace_TextDocumentContent::Result
Server::HandleRequestsWorkspace_TextDocumentContent(lsp::requests::Workspace_TextDocumentContent::Params&& req)
{
    const std::string requested = req.uri.toString();
    static constexpr std::string_view k_scheme = "angelscript-predefined:";
    static constexpr std::string_view k_virtual_scheme = "angelscript-virtual:";

    if (requested.starts_with(k_virtual_scheme))
    {
        lsp::TextDocumentContentResult result;
        result.text = GenerateVirtualMixinDocument(requested);
        return result;
    }

    if (!requested.starts_with(k_scheme))
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "Unsupported scheme: " + requested);
    }

    const std::string wanted = angel_lsp::utils::IncludeResolver::NormalizePath(
        std::string(std::string_view(requested).substr(k_scheme.size())));

    std::string fileUri;
    if (const auto uriOpt = m_predefinedManager.GetUriByPath(wanted))
    {
        fileUri = *uriOpt;
    }

    if (fileUri.empty())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "No predefined stub is loaded for: " + requested);
    }

    std::ifstream file(angel_lsp::utils::UriToPath(fileUri), std::ios::binary);
    if (!file.is_open())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                "The predefined stub could not be read: " + requested);
    }

    lsp::TextDocumentContentResult result;
    result.text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return result;
}

lsp::requests::Workspace_Symbol::Result
Server::HandleRequestsWorkspace_Symbol(lsp::requests::Workspace_Symbol::Params&& req)
{
    if (!m_config.features.enableWorkspaceSymbols)
    {
        return lsp::Array<lsp::SymbolInformation>{};
    }
    features::WorkspaceSymbolRequest wr{req.query, m_symbolTable};
    auto symbols = features::GetWorkspaceSymbols(wr);
    if (symbols.has_value())
    {
        EncodeAcrossDocuments(symbols.value());
        return symbols.value();
    }
    return lsp::Array<lsp::SymbolInformation>{};
}

void Server::RegisterLifecycleHandlers()
{
    m_messageHandler->add<lsp::requests::Initialize>([this](lsp::requests::Initialize::Params&& params)
                                                     { return HandleRequestsInitialized(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Initialized>([this](lsp::notifications::Initialized::Params&& params)
                                                           { HandleNotificationsInitialized(std::move(params)); });

    m_messageHandler->add<lsp::requests::Shutdown>([this]() { return HandleRequestsShutdown(); });

    m_messageHandler->add<lsp::notifications::Exit>([this]() { HandleNotificationsExit(); });

    m_messageHandler->add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
        [this](lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params)
        { HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params)); });

    m_messageHandler->add<lsp::notifications::SetTrace>([this](lsp::notifications::SetTrace::Params&& params)
                                                        { HandleNotificationsSetTrace(std::move(params)); });

    m_messageHandler->add<lsp::notifications::CancelRequest>(
        [](lsp::notifications::CancelRequest::Params&&)
        {
            // Synchronous message dispatch consumes the cancel without further action.
        });
}

void Server::RegisterFileEventHandlers()
{
    m_messageHandler->add<lsp::notifications::Workspace_DidCreateFiles>(
        [this](lsp::notifications::Workspace_DidCreateFiles::Params&& params)
        { HandleNotificationsWorkspace_DidCreateFiles(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Workspace_DidRenameFiles>(
        [this](lsp::notifications::Workspace_DidRenameFiles::Params&& params)
        { HandleNotificationsWorkspace_DidRenameFiles(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Workspace_DidDeleteFiles>(
        [this](lsp::notifications::Workspace_DidDeleteFiles::Params&& params)
        { HandleNotificationsWorkspace_DidDeleteFiles(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
        [this](lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params)
        { HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
        [this](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params)
        { HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params)); });
}

void Server::RegisterDocumentSyncHandlers()
{
    m_messageHandler->add<lsp::notifications::TextDocument_DidOpen>(
        [this](lsp::notifications::TextDocument_DidOpen::Params&& params)
        { HandleNotificationsTextDocument_DidOpen(std::move(params)); });

    m_messageHandler->add<lsp::notifications::TextDocument_DidChange>(
        [this](lsp::notifications::TextDocument_DidChange::Params&& params)
        { HandleNotificationsTextDocument_DidChange(std::move(params)); });

    m_messageHandler->add<lsp::notifications::TextDocument_DidClose>(
        [this](lsp::notifications::TextDocument_DidClose::Params&& params)
        { HandleNotificationsTextDocument_DidClose(std::move(params)); });

    m_messageHandler->add<lsp::notifications::TextDocument_WillSave>(
        [this](lsp::notifications::TextDocument_WillSave::Params&& params)
        { HandleNotificationsTextDocument_WillSave(std::move(params)); });

    m_messageHandler->add<lsp::notifications::TextDocument_DidSave>(
        [this](lsp::notifications::TextDocument_DidSave::Params&& params)
        { HandleNotificationsTextDocument_DidSave(std::move(params)); });

    m_messageHandler->add<lsp::requests::TextDocument_WillSaveWaitUntil>(
        [this](lsp::requests::TextDocument_WillSaveWaitUntil::Params&& req)
        { return HandleRequestsTextDocument_WillSaveWaitUntil(std::move(req)); });
}

void Server::RegisterWorkspaceActionHandlers()
{
    m_messageHandler->add<lsp::requests::Workspace_Diagnostic>(
        [this](lsp::requests::Workspace_Diagnostic::Params&& params)
        { return HandleRequestsWorkspace_Diagnostic(std::move(params)); });

    m_messageHandler->add<lsp::notifications::Workspace_DidChangeConfiguration>(
        [this](lsp::notifications::Workspace_DidChangeConfiguration::Params&& params)
        { HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params)); });

    m_messageHandler->add<lsp::requests::Workspace_TextDocumentContent>(
        [this](lsp::requests::Workspace_TextDocumentContent::Params&& req)
        { return HandleRequestsWorkspace_TextDocumentContent(std::move(req)); });

    m_messageHandler->add("angelscript/virtualDocumentContent", [this](lsp::json::Value&& params)
                          { return HandleRequestsVirtualDocumentContent(std::move(params)); });

    m_messageHandler->add<lsp::requests::Workspace_Symbol>([this](lsp::requests::Workspace_Symbol::Params&& req)
                                                           { return HandleRequestsWorkspace_Symbol(std::move(req)); });

    m_messageHandler->add<lsp::requests::WorkspaceSymbol_Resolve>(
        [this](lsp::requests::WorkspaceSymbol_Resolve::Params&& req)
        { return HandleRequestsWorkspaceSymbol_Resolve(std::move(req)); });

    m_messageHandler->add<lsp::requests::Workspace_ExecuteCommand>(
        [this](lsp::requests::Workspace_ExecuteCommand::Params&& req)
        { return HandleRequestsWorkspace_ExecuteCommand(std::move(req)); });
}

void Server::RegisterWorkspaceHandlers()
{
    RegisterLifecycleHandlers();
    RegisterFileEventHandlers();
    RegisterDocumentSyncHandlers();
    RegisterWorkspaceActionHandlers();
}

void Server::UpdateFormatConfiguration(const lsp::LSPObject& section)
{
    if (const auto* formatVal = section.find("format"); formatVal && formatVal->isObject())
    {
        if (const auto* styleVal = formatVal->object().find("braceStyle"); styleVal && styleVal->isString())
        {
            const bool wantsKR = BraceStyleIsKR(styleVal->string());
            if (wantsKR != m_formatBraceStyleKR.exchange(wantsKR, std::memory_order_relaxed))
            {
                LogInfo(fmt::format("Format brace style changed to '{}'", styleVal->string()));
            }
        }
    }
}

bool Server::UpdateEngineConfiguration(const lsp::LSPObject& section)
{
    bool engineChanged = false;
    const lsp::LSPObject* engineObj = nullptr;
    if (const auto* e = section.find("engine"); e && e->isObject())
    {
        engineObj = &e->object();
    }

    auto applyBool = [&](std::string_view name, bool& target)
    {
        if (auto v = FindSectionBool(section, engineObj, name); v && target != *v)
        {
            target = *v;
            engineChanged = true;
        }
    };

    applyBool("foreachSupport", m_config.engine.foreachSupport);
    applyBool("requireEnumScope", m_config.engine.requireEnumScope);
    applyBool("alwaysImplDefaultConstruct", m_config.engine.alwaysImplDefaultConstruct);
    applyBool("allowUnicodeIdentifiers", m_config.engine.allowUnicodeIdentifiers);
    applyBool("ignoreDuplicateSharedIntf", m_config.engine.ignoreDuplicateSharedIntf);

    if (auto v = FindSectionInt(section, engineObj, "compilerWarnings"); v && m_config.engine.compilerWarnings != *v)
    {
        m_config.engine.compilerWarnings = *v;
        engineChanged = true;
    }

    return engineChanged;
}

void Server::UpdateFeatureConfiguration(const lsp::LSPObject& section)
{
    if (const auto* featVal = section.find("features"); featVal && featVal->isObject())
    {
        if (const auto* vmd = featVal->object().find("enableVirtualMixinDocuments"); vmd && vmd->isBoolean())
        {
            m_config.features.enableVirtualMixinDocuments = vmd->boolean();
            m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);
        }
    }
    else if (const auto* vmd = section.find("enableVirtualMixinDocuments"); vmd && vmd->isBoolean())
    {
        m_config.features.enableVirtualMixinDocuments = vmd->boolean();
        m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);
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

bool Server::UpdateModulesConfiguration(const lsp::LSPObject& section)
{
    bool entryPointChanged = false;
    if (const auto* entryVal = section.find("moduleEntryPoint"); entryVal && entryVal->isString())
    {
        if (entryVal->string() != m_config.moduleEntryPoint)
        {
            m_config.moduleEntryPoint = entryVal->string();
            entryPointChanged = true;
            LogInfo(fmt::format("Module entry point changed to '{}'", m_config.moduleEntryPoint));
        }
    }

    const auto* modulesVal = section.find("modules");
    if (!modulesVal || !modulesVal->isArray())
    {
        return entryPointChanged;
    }

    auto parsed = ParseModuleDefinitions(modulesVal->array());

    const bool changed =
        parsed.size() != m_config.modules.size() ||
        !std::equal(parsed.begin(), parsed.end(), m_config.modules.begin(),
                    [](const config::ServerConfig::ModuleDefinition& a, const config::ServerConfig::ModuleDefinition& b)
                    { return a.name == b.name && a.entry == b.entry && a.folder == b.folder; });

    if (changed)
    {
        m_config.modules = std::move(parsed);
        LogInfo(fmt::format("Modules changed ({} configured); rescanning", m_config.modules.size()));
        return true;
    }
    return entryPointChanged;
}

bool Server::UpdateIncludeConfiguration(const lsp::LSPObject& section)
{
    bool changed = false;
    if (const auto* includeVal = section.find("include"); includeVal && includeVal->isObject())
    {
        if (const auto* implicitVal = includeVal->object().find("implicitExtension");
            implicitVal && implicitVal->isBoolean())
        {
            if (implicitVal->boolean() != m_config.implicitIncludeExtension)
            {
                m_config.implicitIncludeExtension = implicitVal->boolean();
                LogInfo(fmt::format("Implicit include extension {}; rebuilding the include graph",
                                    m_config.implicitIncludeExtension ? "on" : "off"));
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
        if (updated != m_config.forceIncludeFiles)
        {
            m_config.forceIncludeFiles = std::move(updated);
            LogInfo(fmt::format("Force include files changed ({} entries)", m_config.forceIncludeFiles.size()));
            changed = true;
        }
    }
    return changed;
}

bool Server::UpdatePredefinedAndProfileConfiguration(const lsp::LSPObject& section)
{
    bool shouldRescan = false;

    if (const auto* predefinedVal = section.find("predefined"); predefinedVal && predefinedVal->isObject())
    {
        if (const auto* activeVal = predefinedVal->object().find("active"); activeVal && activeVal->isString())
        {
            if (activeVal->string() != m_config.activePredefined)
            {
                m_config.activePredefined = activeVal->string();
                LogInfo(
                    fmt::format("Active predefined stub changed to '{}'; rescanning",
                                m_config.activePredefined.empty() ? std::string("<all>") : m_config.activePredefined));
                shouldRescan = true;
            }
        }
    }

    if (const auto* profileVal = section.find("engineProfile"); profileVal && profileVal->isString())
    {
        if (!profileVal->string().empty() && profileVal->string() != EngineProfile())
        {
            {
                std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                m_engineProfile = profileVal->string();
            }
            LogInfo(fmt::format("Engine profile changed to '{}'; reloading predefineds", profileVal->string()));
            shouldRescan = true;
        }
    }

    return shouldRescan;
}

bool Server::UpdateSearchDirectoriesConfiguration(const lsp::LSPObject& section)
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

    if (updated != *SearchDirectories())
    {
        const size_t count = updated.size();
        {
            std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
            m_searchDirectories = std::make_shared<const std::vector<std::string>>(std::move(updated));
        }
        LogInfo(fmt::format("Search directories changed ({} entries); rebuilding the include graph", count));
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

    UpdateFormatConfiguration(*section);
    if (UpdateEngineConfiguration(*section))
    {
        ReanalyseOpenDocuments();
    }
    UpdateFeatureConfiguration(*section);

    bool shouldRescan = false;
    shouldRescan = UpdateModulesConfiguration(*section) || shouldRescan;
    shouldRescan = UpdateIncludeConfiguration(*section) || shouldRescan;
    shouldRescan = UpdatePredefinedAndProfileConfiguration(*section) || shouldRescan;
    shouldRescan = UpdateSearchDirectoriesConfiguration(*section) || shouldRescan;

    if (shouldRescan)
    {
        RestartWorkspaceScan();
    }
}

lsp::requests::Workspace_Diagnostic::Result
Server::HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params&& params)
{
    if (!m_config.features.enablePullDiagnostics)
    {
        return lsp::WorkspaceDiagnosticReport{};
    }

    // What the client already holds, so an unedited document can be answered with its id alone.
    ankerl::unordered_dense::map<std::string, std::string> known;
    for (const auto& previous : params.previousResultIds)
        known[DocumentKey(previous.uri.toString())] = previous.value;

    lsp::WorkspaceDiagnosticReport report;

    std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
    for (const auto& [uriStr, snapshot] : m_diagnosticsCache)
    {
        // Out under the client's own spelling, for the same reason PublishDiagnostics does it:
        // the key is canonical and the client matches these against its own URIs.
        const std::string outgoingUri = m_documentStore.GetClientUri(uriStr);

        if (const auto it = known.find(uriStr); it != known.end() && it->second == snapshot.resultId)
        {
            lsp::WorkspaceUnchangedDocumentDiagnosticReport unchanged;
            unchanged.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
            unchanged.version = (snapshot.version >= 0) ? lsp::NullOr<int>(snapshot.version) : nullptr;
            unchanged.resultId = snapshot.resultId;
            report.items.push_back(std::move(unchanged));
            continue;
        }

        lsp::WorkspaceFullDocumentDiagnosticReport full;
        full.uri = lsp::DocumentUri(lsp::Uri::parse(outgoingUri));
        full.version = (snapshot.version >= 0) ? lsp::NullOr<int>(snapshot.version) : nullptr;
        full.resultId = snapshot.resultId;
        full.items = snapshot.items;
        report.items.push_back(std::move(full));
    }

    return report;
}

lsp::requests::WorkspaceSymbol_Resolve::Result
Server::HandleRequestsWorkspaceSymbol_Resolve(lsp::requests::WorkspaceSymbol_Resolve::Params&& params)
{
    if (!m_config.features.enableWorkspaceSymbols)
    {
        return std::move(params);
    }

    // Workspace symbols are indexed and returned with their full container and location
    // information in workspace/symbol. This handler returns the symbol unchanged so
    // clients resolving workspace symbols receive a valid response instead of MethodNotFound.
    return std::move(params);
}

lsp::requests::Workspace_ExecuteCommand::Result
Server::ExecuteFormatPredefinedStub(const std::optional<lsp::Array<lsp::LSPAny>>& args)
{
    if (!args || args->empty() || !args->front().isString())
    {
        LogError("angelscript.formatPredefinedStub needs the stub's URI as its argument");
        return lsp::Null{};
    }

    const std::string uriStr = args->front().string();
    std::string text;

    if (const auto doc = LookupOpenDocument(uriStr); doc && doc->text)
    {
        text = *doc->text;
    }
    else
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            LogError(fmt::format("Cannot open predefined file to format: {}", path));
            return lsp::Null{};
        }
        text.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    angel_lsp::parser::AngelScriptParser formatterParser(m_logger.get());
    std::string formatted = angel_lsp::features::formatting::FormatPredefinedStub(text, formatterParser);

    const bool changed = (formatted != text);
    LogInfo(fmt::format("Formatted predefined stub {}: {}", uriStr, changed ? "rewritten" : "already formatted"));

    lsp::json::Object answer;
    answer["changed"] = lsp::json::Value(changed);
    answer["text"] = lsp::json::Value(std::move(formatted));
    return lsp::json::Value(std::move(answer));
}

lsp::requests::Workspace_ExecuteCommand::Result Server::ExecuteListPredefinedStubs() const
{
    lsp::json::Object answer;
    lsp::json::Array paths;
    std::string effective;

    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        for (const auto& path : m_discoveredPredefined)
        {
            paths.push_back(lsp::json::Value(std::string(path)));
        }
        effective = m_effectivePredefined;
    }

    answer["stubs"] = std::move(paths);
    answer["active"] = std::move(effective);
    answer["merging"] = (m_config.activePredefined == "all");

    return lsp::json::Value(std::move(answer));
}

lsp::requests::Workspace_ExecuteCommand::Result
Server::HandleRequestsWorkspace_ExecuteCommand(lsp::requests::Workspace_ExecuteCommand::Params&& params)
{
    if (params.command == "angelscript.rescanWorkspace")
    {
        RestartWorkspaceScan();
        return lsp::Null{};
    }

    if (params.command == "angelscript.formatPredefinedStub")
    {
        return ExecuteFormatPredefinedStub(params.arguments);
    }

    if (params.command == "angelscript.listPredefinedStubs")
    {
        return ExecuteListPredefinedStubs();
    }

    throw lsp::RequestError(lsp::MessageError::InvalidParams, "Unknown command: " + params.command);
}

} // namespace angel_lsp
