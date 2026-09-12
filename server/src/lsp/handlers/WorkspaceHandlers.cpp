#include "lsp/Server.h"
#include "utils/Utils.h"
#include "lsp/PositionCodec.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "features/formatting/PredefinedStubFormatter.h"
#include <fstream>
#include <cctype>

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
    }

    void Server::RegisterWorkspaceHandlers()
    {
        m_messageHandler->add<lsp::requests::Initialize>(
            [this](lsp::requests::Initialize::Params &&params)
            {
                return this->HandleRequestsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Initialized>(
            [this](lsp::notifications::Initialized::Params &&params)
            {
                this->HandleNotificationsInitialized(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Shutdown>(
            [this]()
            {
                return this->HandleRequestsShutdown();
            });

        m_messageHandler->add<lsp::notifications::Exit>(
            [this]()
            {
                this->HandleNotificationsExit();
            });

        m_messageHandler->add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
            [this](lsp::notifications::Window_WorkDoneProgress_Cancel::Params &&params)
            {
                this->HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::SetTrace>(
            [this](lsp::notifications::SetTrace::Params &&params)
            {
                this->HandleNotificationsSetTrace(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidCreateFiles>(
            [this](lsp::notifications::Workspace_DidCreateFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidCreateFiles(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Workspace_Diagnostic>(
            [this](lsp::requests::Workspace_Diagnostic::Params &&params)
            {
                return this->HandleRequestsWorkspace_Diagnostic(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeConfiguration>(
            [this](lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidRenameFiles>(
            [this](lsp::notifications::Workspace_DidRenameFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidRenameFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidDeleteFiles>(
            [this](lsp::notifications::Workspace_DidDeleteFiles::Params &&params)
            {
                HandleNotificationsWorkspace_DidDeleteFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
            [this](lsp::notifications::Workspace_DidChangeWatchedFiles::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
            [this](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params &&params)
            {
                this->HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_WillSave>(
            [this](lsp::notifications::TextDocument_WillSave::Params &&params)
            {
                this->HandleNotificationsTextDocument_WillSave(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidSave>(
            [this](lsp::notifications::TextDocument_DidSave::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidSave(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidOpen>(
            [this](lsp::notifications::TextDocument_DidOpen::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidOpen(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidChange>(
            [this](lsp::notifications::TextDocument_DidChange::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidChange(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::TextDocument_DidClose>(
            [this](lsp::notifications::TextDocument_DidClose::Params &&params)
            {
                this->HandleNotificationsTextDocument_DidClose(std::move(params));
            });

        m_messageHandler->add<lsp::requests::Workspace_TextDocumentContent>(
            [this](lsp::requests::Workspace_TextDocumentContent::Params &&req) -> lsp::requests::Workspace_TextDocumentContent::Result
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
                    throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                            "Unsupported scheme: " + requested);
                }

                const std::string wanted = angel_lsp::utils::IncludeResolver::NormalizePath(
                    std::string(std::string_view(requested).substr(k_scheme.size())));

                std::string fileUri;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    for (const auto &entry : m_predefinedUriByPath)
                    {
                        if (entry.first == wanted)
                        {
                            fileUri = entry.second;
                            break;
                        }
                    }
                }

                if (fileUri.empty())
                {
                    throw lsp::RequestError(lsp::MessageError::InvalidParams,
                                            "No predefined stub is loaded for: " + requested);
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
            });

        m_messageHandler->add("angelscript/virtualDocumentContent",
            [this](lsp::json::Value &&params) -> lsp::json::Value
            {
                return this->HandleRequestsVirtualDocumentContent(std::move(params));
            });

        m_messageHandler->add<lsp::notifications::CancelRequest>(
            [](lsp::notifications::CancelRequest::Params &&)
            {
                // Synchronous message dispatch consumes the cancel without further action.
            });

        m_messageHandler->add<lsp::requests::Workspace_Symbol>(
            [this](lsp::requests::Workspace_Symbol::Params &&req) -> lsp::requests::Workspace_Symbol::Result
            {
                if (!m_config.features.enableWorkspaceSymbols)
                {
                    return lsp::Array<lsp::SymbolInformation>{};
                }
                features::WorkspaceSymbolRequest wr{ req.query, m_symbolTable };
                auto symbols = features::GetWorkspaceSymbols(wr);
                if (symbols.has_value())
                {
                    EncodeAcrossDocuments(symbols.value());
                    return symbols.value();
                }
                return lsp::Array<lsp::SymbolInformation>{};
            });

        m_messageHandler->add<lsp::requests::WorkspaceSymbol_Resolve>(
            [this](lsp::requests::WorkspaceSymbol_Resolve::Params &&req) -> lsp::requests::WorkspaceSymbol_Resolve::Result
            {
                return this->HandleRequestsWorkspaceSymbol_Resolve(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_WillSaveWaitUntil>(
            [this](lsp::requests::TextDocument_WillSaveWaitUntil::Params &&req) -> lsp::requests::TextDocument_WillSaveWaitUntil::Result
            {
                return this->HandleRequestsTextDocument_WillSaveWaitUntil(std::move(req));
            });

        m_messageHandler->add<lsp::requests::Workspace_ExecuteCommand>(
            [this](lsp::requests::Workspace_ExecuteCommand::Params &&req) -> lsp::requests::Workspace_ExecuteCommand::Result
            {
                return this->HandleRequestsWorkspace_ExecuteCommand(std::move(req));
            });
    }

    void Server::HandleNotificationsWorkspace_DidChangeConfiguration(lsp::notifications::Workspace_DidChangeConfiguration::Params &&params)
    {
        // The client synchronises the whole "angelscript" section (see the LanguageClient's
        // synchronize.configurationSection), so settings arrives either as that section directly or
        // wrapped in an object keyed by it, depending on the client. Both shapes are accepted.
        if (!params.settings.isObject())
        {
            return;
        }

        const lsp::LSPObject *section = &params.settings.object();
        if (const auto *nested = section->find("angelscript"); nested && nested->isObject())
        {
            section = &nested->object();
        }

        bool shouldRescan = false;

        // Nested one level down, because the client sends it as `angelscript.format.braceStyle`.
        // Nothing to rescan: it changes only what the next format request produces.
        if (const auto *formatVal = section->find("format"); formatVal && formatVal->isObject())
        {
            if (const auto *styleVal = formatVal->object().find("braceStyle");
                styleVal && styleVal->isString())
            {
                const bool wantsKR = BraceStyleIsKR(styleVal->string());
                if (wantsKR != m_formatBraceStyleKR.exchange(wantsKR, std::memory_order_relaxed))
                {
                    LogInfo(fmt::format("Format brace style changed to '{}'", styleVal->string()));
                }
            }
        }

        bool engineChanged = false;
        const lsp::LSPObject *engineObj = nullptr;
        if (const auto *e = section->find("engine"); e && e->isObject())
        {
            engineObj = &e->object();
        }

        auto getEngineBool = [&](std::string_view name) -> std::optional<bool> {
            if (engineObj)
            {
                if (const auto *val = engineObj->find(std::string(name)); val && val->isBoolean())
                    return val->boolean();
            }
            std::string dotKey = "engine." + std::string(name);
            if (const auto *val = section->find(dotKey); val && val->isBoolean())
                return val->boolean();
            std::string fullKey = "angelscript.engine." + std::string(name);
            if (const auto *val = section->find(fullKey); val && val->isBoolean())
                return val->boolean();
            return std::nullopt;
        };

        auto getEngineInt = [&](std::string_view name) -> std::optional<int> {
            if (engineObj)
            {
                if (const auto *val = engineObj->find(std::string(name)); val && val->isNumber())
                    return static_cast<int>(val->number());
            }
            std::string dotKey = "engine." + std::string(name);
            if (const auto *val = section->find(dotKey); val && val->isNumber())
                return static_cast<int>(val->number());
            std::string fullKey = "angelscript.engine." + std::string(name);
            if (const auto *val = section->find(fullKey); val && val->isNumber())
                return static_cast<int>(val->number());
            return std::nullopt;
        };

        if (auto v = getEngineBool("foreachSupport"))
        {
            if (m_config.engine.foreachSupport != *v)
            {
                m_config.engine.foreachSupport = *v;
                engineChanged = true;
            }
        }
        if (auto v = getEngineBool("requireEnumScope"))
        {
            if (m_config.engine.requireEnumScope != *v)
            {
                m_config.engine.requireEnumScope = *v;
                engineChanged = true;
            }
        }
        if (auto v = getEngineBool("alwaysImplDefaultConstruct"))
        {
            if (m_config.engine.alwaysImplDefaultConstruct != *v)
            {
                m_config.engine.alwaysImplDefaultConstruct = *v;
                engineChanged = true;
            }
        }
        if (auto v = getEngineBool("allowUnicodeIdentifiers"))
        {
            if (m_config.engine.allowUnicodeIdentifiers != *v)
            {
                m_config.engine.allowUnicodeIdentifiers = *v;
                engineChanged = true;
            }
        }
        if (auto v = getEngineBool("ignoreDuplicateSharedIntf"))
        {
            if (m_config.engine.ignoreDuplicateSharedIntf != *v)
            {
                m_config.engine.ignoreDuplicateSharedIntf = *v;
                engineChanged = true;
            }
        }
        if (auto v = getEngineInt("compilerWarnings"))
        {
            if (m_config.engine.compilerWarnings != *v)
            {
                m_config.engine.compilerWarnings = *v;
                engineChanged = true;
            }
        }

        if (engineChanged)
        {
            ReanalyseOpenDocuments();
        }

        if (const auto *featVal = section->find("features"); featVal && featVal->isObject())
        {
            if (const auto *vmd = featVal->object().find("enableVirtualMixinDocuments"); vmd && vmd->isBoolean())
            {
                m_config.features.enableVirtualMixinDocuments = vmd->boolean();
                m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);
            }
        }
        else if (const auto *vmd = section->find("enableVirtualMixinDocuments"); vmd && vmd->isBoolean())
        {
            m_config.features.enableVirtualMixinDocuments = vmd->boolean();
            m_symbolTable.SetVirtualMixinDocumentsEnabled(m_config.features.enableVirtualMixinDocuments);
        }

        // The stub selection rides the same rescan the engine profile does. With a working unload
        // path the rescan is enough: the stub that stops being active is dropped and the new one
        // collected, without restarting the server.
        // A change here changes which files every `#include` resolves to, so the include graph
        // has to be rebuilt - the same rescan the stub selection and the engine profile ride.
        // Modules decide which files are analysed at all and what `external shared` may refer to,
        // so a change here has to rebuild the index and re-walk the workspace. Without this the
        // setting only took effect on the next restart - and a folder renamed on disk, which is
        // the same edit from the server's point of view, never took effect at all.
        if (const auto *modulesVal = section->find("modules"); modulesVal && modulesVal->isArray())
        {
            std::vector<config::ServerConfig::ModuleDefinition> parsed;

            for (const auto &item : modulesVal->array())
            {
                if (!item.isObject())
                    continue;

                config::ServerConfig::ModuleDefinition definition;

                if (const auto *nameVal = item.object().find("name"); nameVal && nameVal->isString())
                    definition.name = nameVal->string();
                if (const auto *entryVal = item.object().find("entry"); entryVal && entryVal->isString())
                    definition.entry = entryVal->string();
                if (const auto *folderVal = item.object().find("folder"); folderVal && folderVal->isString())
                    definition.folder = folderVal->string();

                parsed.push_back(std::move(definition));
            }

            const bool changed =
                parsed.size() != m_config.modules.size() ||
                !std::equal(parsed.begin(), parsed.end(), m_config.modules.begin(),
                            [](const config::ServerConfig::ModuleDefinition &a,
                               const config::ServerConfig::ModuleDefinition &b)
                            { return a.name == b.name && a.entry == b.entry && a.folder == b.folder; });

            if (changed)
            {
                m_config.modules = std::move(parsed);
                LogInfo(fmt::format("Modules changed ({} configured); rescanning",
                                              m_config.modules.size()));
                shouldRescan = true;
            }
        }

        if (const auto *includeVal = section->find("include"); includeVal && includeVal->isObject())
        {
            if (const auto *implicitVal = includeVal->object().find("implicitExtension");
                implicitVal && implicitVal->isBoolean())
            {
                if (implicitVal->boolean() != m_config.implicitIncludeExtension)
                {
                    m_config.implicitIncludeExtension = implicitVal->boolean();
                    LogInfo(fmt::format("Implicit include extension {}; rebuilding the include graph",
                                                  m_config.implicitIncludeExtension ? "on" : "off"));
                    shouldRescan = true;
                }
            }
        }

        if (const auto *predefinedVal = section->find("predefined"); predefinedVal && predefinedVal->isObject())
        {
            if (const auto *activeVal = predefinedVal->object().find("active");
                activeVal && activeVal->isString())
            {
                if (activeVal->string() != m_config.activePredefined)
                {
                    m_config.activePredefined = activeVal->string();
                    LogInfo(fmt::format("Active predefined stub changed to '{}'; rescanning",
                                                  m_config.activePredefined.empty()
                                                      ? std::string("<all>")
                                                      : m_config.activePredefined));
                    shouldRescan = true;
                }
            }
        }

        if (const auto *profileVal = section->find("engineProfile"); profileVal && profileVal->isString())
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

        const auto *directories = section->find("searchDirectories");
        if (directories && directories->isArray())
        {
            std::vector<std::string> updated;
            for (const auto &entry : directories->array())
            {
                if (entry.isString() && !entry.string().empty())
                    updated.push_back(entry.string());
            }

            if (updated != *SearchDirectories())
            {
                const size_t count = updated.size();

                // Swapped in as a whole new list rather than assigned into the old one: a worker
                // holding the previous handle keeps reading that revision safely until it is done,
                // and the old buffer is freed only when the last of them lets go.
                {
                    std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                    m_searchDirectories = std::make_shared<const std::vector<std::string>>(std::move(updated));
                }

                LogInfo(fmt::format("Search directories changed ({} entries); rebuilding the include graph", count));
                shouldRescan = true;
            }
        }

        if (shouldRescan)
        {
            // Which files a directive resolves to depends entirely on these paths/profiles, so every edge in the
            // graph is now suspect.
            RestartWorkspaceScan();
        }
    }


    lsp::requests::Workspace_Diagnostic::Result Server::HandleRequestsWorkspace_Diagnostic(lsp::requests::Workspace_Diagnostic::Params &&params)
    {
        if (!m_config.features.enablePullDiagnostics)
        {
            return lsp::WorkspaceDiagnosticReport{};
        }

        // What the client already holds, so an unedited document can be answered with its id alone.
        ankerl::unordered_dense::map<std::string, std::string> known;
        for (const auto &previous : params.previousResultIds)
            known[DocumentKey(previous.uri.toString())] = previous.value;

        lsp::WorkspaceDiagnosticReport report;

        std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
        for (const auto &[uriStr, snapshot] : m_diagnosticsCache)
        {
            // Out under the client's own spelling, for the same reason PublishDiagnostics does it:
            // the key is canonical and the client matches these against its own URIs.
            const auto clientUri = m_clientUriByKey.find(uriStr);
            const std::string &outgoingUri = clientUri != m_clientUriByKey.end() ? clientUri->second : uriStr;

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


    lsp::requests::WorkspaceSymbol_Resolve::Result Server::HandleRequestsWorkspaceSymbol_Resolve(lsp::requests::WorkspaceSymbol_Resolve::Params &&params)
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


    lsp::requests::Workspace_ExecuteCommand::Result Server::HandleRequestsWorkspace_ExecuteCommand(lsp::requests::Workspace_ExecuteCommand::Params &&params)
    {
        if (params.command == "angelscript.rescanWorkspace")
        {
            RestartWorkspaceScan();
            return lsp::Null{};
        }

        if (params.command == "angelscript.formatPredefinedStub")
        {
            // Takes one argument, the stub's URI, and answers with its formatted text. The client
            // applies the edit, which is what lets this work on a stub that is not open: the server
            // reads it from disk when no buffer holds it.
            //
            // Answering with text rather than a WorkspaceEdit keeps the decision on the client
            // side, where the user is: an edit the server pushed would rewrite a file the moment
            // the command ran, with no editor to undo it in if the stub was not open.
            if (!params.arguments || params.arguments->empty() || !params.arguments->front().isString())
            {
                LogError("angelscript.formatPredefinedStub needs the stub's URI as its argument");
                return lsp::Null{};
            }

            const std::string uriStr = params.arguments->front().string();
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

            // A parser of its own: this runs on the message loop, and m_parser holds the trees the
            // open documents are using.
            angel_lsp::parser::AngelScriptParser formatterParser(m_logger.get());
            std::string formatted =
                angel_lsp::features::formatting::FormatPredefinedStub(text, formatterParser);

            const bool changed = formatted != text;
            LogInfo(fmt::format("Formatted predefined stub {}: {}",
                                          uriStr, changed ? "rewritten" : "already formatted"));

            lsp::json::Object answer;
            answer["changed"] = lsp::json::Value(changed);
            answer["text"] = lsp::json::Value(std::move(formatted));
            return lsp::json::Value(std::move(answer));
        }

        if (params.command == "angelscript.listPredefinedStubs")
        {
            // The client's stub picker asks for this rather than scanning itself. Whether a file
            // is a stub is this server's rule, and it is not a rule the client can guess: a file
            // named exactly `as.predefined` counts, and so does any name ending in the configured
            // suffix. One answer, one place.
            lsp::json::Object answer;
            lsp::json::Array paths;

            std::string effective;

            {
                std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
                for (const auto &path : m_discoveredPredefined)
                {
                    // The Value constructor takes its string by rvalue, so the copy is explicit.
                    paths.push_back(lsp::json::Value(std::string(path)));
                }
                effective = m_effectivePredefined;
            }

            answer["stubs"] = std::move(paths);

            // What is loaded, not what was configured. With nothing configured the scan picks one,
            // and a picker showing "none selected" next to a workspace that plainly has host types
            // would be telling the user something untrue.
            answer["active"] = std::move(effective);
            answer["merging"] = m_config.activePredefined == "all";

            return lsp::json::Value(std::move(answer));
        }

        throw lsp::RequestError(lsp::MessageError::InvalidParams, "Unknown command: " + params.command);
    }


}
