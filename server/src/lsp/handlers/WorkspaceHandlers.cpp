#include "features/formatting/PredefinedStubFormatter.h"
#include "features/workspace_symbol/WorkspaceSymbolHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Utils.h"
#include <cctype>
#include <fstream>

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

static std::optional<bool> FindSectionBool(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                           std::string_view name, std::string_view prefix = "engine")
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isBoolean())
        {
            return val->boolean();
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    return std::nullopt;
}

static std::optional<int> FindSectionInt(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                         std::string_view name, std::string_view prefix = "engine")
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isNumber())
        {
            return static_cast<int>(val->number());
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}

static std::optional<std::string> FindSectionString(const lsp::LSPObject& section, const lsp::LSPObject* obj,
                                                    std::string_view name, std::string_view prefix = "engine")
{
    if (obj)
    {
        if (const auto* val = obj->find(std::string(name)); val && val->isString())
        {
            return val->string();
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isString())
    {
        return val->string();
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isString())
    {
        return val->string();
    }
    return std::nullopt;
}

static std::optional<std::vector<std::string>> FindSectionStringArray(const lsp::LSPObject& section,
                                                                      const lsp::LSPObject* obj, std::string_view name,
                                                                      std::string_view prefix)
{
    const lsp::json::Value* arrayVal = nullptr;
    if (obj)
    {
        if (const auto* val = obj->find(std::string(name)); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        const std::string dotKey = std::string(prefix) + "." + std::string(name);
        if (const auto* val = section.find(dotKey); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
        if (const auto* val = section.find(fullKey); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        return std::nullopt;
    }
    std::vector<std::string> items;
    for (const auto& entry : arrayVal->array())
    {
        if (entry.isString() && !entry.string().empty())
        {
            items.push_back(entry.string());
        }
    }
    return items;
}

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

static void RegisterLifecycleHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::Initialize>([&server](lsp::requests::Initialize::Params&& params)
                                           { return server.HandleRequestsInitialized(std::move(params)); });

    handler.add<lsp::notifications::Initialized>([&server](lsp::notifications::Initialized::Params&& params)
                                                 { server.HandleNotificationsInitialized(std::move(params)); });

    handler.add<lsp::requests::Shutdown>([&server]() { return server.HandleRequestsShutdown(); });

    handler.add<lsp::notifications::Exit>([&server]() { server.HandleNotificationsExit(); });

    handler.add<lsp::notifications::Window_WorkDoneProgress_Cancel>(
        [&server](lsp::notifications::Window_WorkDoneProgress_Cancel::Params&& params)
        { server.HandleNotificationsWindow_WorkDoneProgress_Cancel(std::move(params)); });

    handler.add<lsp::notifications::SetTrace>([&server](lsp::notifications::SetTrace::Params&& params)
                                              { server.HandleNotificationsSetTrace(std::move(params)); });

    handler.add<lsp::notifications::CancelRequest>(
        [](lsp::notifications::CancelRequest::Params&&)
        {
            // Synchronous message dispatch consumes the cancel without further action.
        });
}

static void RegisterFileEventHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::notifications::Workspace_DidCreateFiles>(
        [&server](lsp::notifications::Workspace_DidCreateFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidCreateFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidRenameFiles>(
        [&server](lsp::notifications::Workspace_DidRenameFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidRenameFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidDeleteFiles>(
        [&server](lsp::notifications::Workspace_DidDeleteFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidDeleteFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeWatchedFiles>(
        [&server](lsp::notifications::Workspace_DidChangeWatchedFiles::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeWatchedFiles(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeWorkspaceFolders>(
        [&server](lsp::notifications::Workspace_DidChangeWorkspaceFolders::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeWorkspaceFolders(std::move(params)); });
}

static void RegisterDocumentSyncHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::notifications::TextDocument_DidOpen>(
        [&server](lsp::notifications::TextDocument_DidOpen::Params&& params)
        { server.HandleNotificationsTextDocument_DidOpen(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidChange>(
        [&server](lsp::notifications::TextDocument_DidChange::Params&& params)
        { server.HandleNotificationsTextDocument_DidChange(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidClose>(
        [&server](lsp::notifications::TextDocument_DidClose::Params&& params)
        { server.HandleNotificationsTextDocument_DidClose(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_WillSave>(
        [&server](lsp::notifications::TextDocument_WillSave::Params&& params)
        { server.HandleNotificationsTextDocument_WillSave(std::move(params)); });

    handler.add<lsp::notifications::TextDocument_DidSave>(
        [&server](lsp::notifications::TextDocument_DidSave::Params&& params)
        { server.HandleNotificationsTextDocument_DidSave(std::move(params)); });

    handler.add<lsp::requests::TextDocument_WillSaveWaitUntil>(
        [&server](lsp::requests::TextDocument_WillSaveWaitUntil::Params&& req)
        { return server.HandleRequestsTextDocument_WillSaveWaitUntil(std::move(req)); });
}

static void RegisterWorkspaceActionHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::Workspace_Diagnostic>(
        [&server](lsp::requests::Workspace_Diagnostic::Params&& params)
        { return server.HandleRequestsWorkspace_Diagnostic(std::move(params)); });

    handler.add<lsp::notifications::Workspace_DidChangeConfiguration>(
        [&server](lsp::notifications::Workspace_DidChangeConfiguration::Params&& params)
        { server.HandleNotificationsWorkspace_DidChangeConfiguration(std::move(params)); });

    handler.add<lsp::requests::Workspace_TextDocumentContent>(
        [&server](lsp::requests::Workspace_TextDocumentContent::Params&& req)
        { return server.HandleRequestsWorkspace_TextDocumentContent(std::move(req)); });

    handler.add("angelscript/virtualDocumentContent", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsVirtualDocumentContent(std::move(params)); });

    handler.add<lsp::requests::Workspace_Symbol>([&server](lsp::requests::Workspace_Symbol::Params&& req)
                                                 { return server.HandleRequestsWorkspace_Symbol(std::move(req)); });

    handler.add<lsp::requests::WorkspaceSymbol_Resolve>(
        [&server](lsp::requests::WorkspaceSymbol_Resolve::Params&& req)
        { return server.HandleRequestsWorkspaceSymbol_Resolve(std::move(req)); });

    handler.add<lsp::requests::Workspace_ExecuteCommand>(
        [&server](lsp::requests::Workspace_ExecuteCommand::Params&& req)
        { return server.HandleRequestsWorkspace_ExecuteCommand(std::move(req)); });
}

void Server::RegisterWorkspaceHandlers()
{
    RegisterLifecycleHandlers(*this, *m_messageHandler);
    RegisterFileEventHandlers(*this, *m_messageHandler);
    RegisterDocumentSyncHandlers(*this, *m_messageHandler);
    RegisterWorkspaceActionHandlers(*this, *m_messageHandler);
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
        features::PointerAlignment align = features::PointerAlignment::Left;
        if (*alignVal == "right" || *alignVal == "Right")
        {
            align = features::PointerAlignment::Right;
        }
        else if (*alignVal == "middle" || *alignVal == "Middle")
        {
            align = features::PointerAlignment::Middle;
        }
        server.GetSessionCoordinator().SetFormatPointerAlignment(align);
    }
}

static bool UpdateEngineConfiguration(Server& server, const lsp::LSPObject& section)
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
    auto applyInt = [&](std::string_view name, int& target)
    {
        if (auto v = FindSectionInt(section, engineObj, name); v && target != *v)
        {
            target = *v;
            engineChanged = true;
        }
    };

    server.BindEngineConfiguration(applyBool, applyInt);

    return engineChanged;
}

static bool UpdateUnusedFunctionExclusions(const lsp::LSPObject& section, const lsp::LSPObject* diagObj,
                                           config::DiagnosticsConfig& diag)
{
    bool changed = false;
    if (auto ignored = FindSectionStringArray(section, diagObj, "ignoredUnusedFunctions", "diagnostics"))
    {
        std::unordered_set<std::string> next(ignored->begin(), ignored->end());
        if (diag.ignoredUnusedFunctions != next)
        {
            diag.ignoredUnusedFunctions = std::move(next);
            changed = true;
        }
    }
    if (auto patterns = FindSectionStringArray(section, diagObj, "ignoredUnusedFunctionPatterns", "diagnostics"))
    {
        if (diag.ignoredUnusedFunctionPatterns != *patterns)
        {
            diag.ignoredUnusedFunctionPatterns = std::move(*patterns);
            changed = true;
        }
    }
    return changed;
}

static void UpdateUnusedDiagnostics(const lsp::LSPObject& section, const lsp::LSPObject* diagObj,
                                    config::DiagnosticsConfig& diag, bool& changed)
{
    auto applyExplicitBool = [&](std::string_view name, bool& target, bool& explicitFlag)
    {
        if (auto v = FindSectionBool(section, diagObj, name, "diagnostics"))
        {
            explicitFlag = true;
            if (target != *v)
            {
                target = *v;
                changed = true;
            }
        }
    };

    applyExplicitBool("reportUnusedFunctions", diag.reportUnusedFunctions, diag.reportUnusedFunctionsExplicit);
    applyExplicitBool("reportUnusedGlobalVariables", diag.reportUnusedGlobalVariables,
                      diag.reportUnusedGlobalVariablesExplicit);
    applyExplicitBool("reportUnusedClasses", diag.reportUnusedClasses, diag.reportUnusedClassesExplicit);
    applyExplicitBool("reportUnusedMembers", diag.reportUnusedMembers, diag.reportUnusedMembersExplicit);
}

static bool UpdateDiagnosticsConfiguration(Server& server, const lsp::LSPObject& section)
{
    bool diagnosticsChanged = false;
    const lsp::LSPObject* diagObj = nullptr;
    if (const auto* d = section.find("diagnostics"); d && d->isObject())
    {
        diagObj = &d->object();
    }

    auto& config = server.GetMutableConfig();

    auto applyBool = [&](std::string_view name, bool& target)
    {
        if (auto v = FindSectionBool(section, diagObj, name, "diagnostics"); v && target != *v)
        {
            target = *v;
            diagnosticsChanged = true;
        }
    };

    applyBool("reportUnknownTypes", config.diagnostics.reportUnknownTypes);
    applyBool("reportAccessorPortability", config.diagnostics.reportAccessorPortability);
    applyBool("reportAccessorDisabled", config.diagnostics.reportAccessorDisabled);
    applyBool("reportBoolConversion", config.diagnostics.reportBoolConversion);
    applyBool("reportMissingFuncdef", config.diagnostics.reportMissingFuncdef);
    applyBool("reportIntegerDivision", config.diagnostics.reportIntegerDivision);
    applyBool("reportPossibleNullDereference", config.diagnostics.reportPossibleNullDereference);
    applyBool("reportAllNullDereferences", config.diagnostics.reportAllNullDereferences);

    UpdateUnusedDiagnostics(section, diagObj, config.diagnostics, diagnosticsChanged);

    if (auto v = FindSectionInt(section, diagObj, "reportHandleComparisonEquality", "diagnostics");
        v && config.diagnostics.reportHandleComparisonEquality != *v)
    {
        config.diagnostics.reportHandleComparisonEquality = *v;
        diagnosticsChanged = true;
    }

    if (auto v = FindSectionString(section, diagObj, "missingAssetPathSeverity", "diagnostics");
        v && config.diagnostics.missingAssetPathSeverity != *v)
    {
        config.diagnostics.missingAssetPathSeverity = *v;
        diagnosticsChanged = true;
    }

    if (UpdateUnusedFunctionExclusions(section, diagObj, config.diagnostics))
    {
        diagnosticsChanged = true;
    }

    return diagnosticsChanged;
}

static void UpdateInlayHintDisplayConfig(const lsp::LSPObject& section, const lsp::LSPObject* ihObj,
                                         config::ServerConfig& cfg)
{
    if (auto maxParam = FindSectionInt(section, ihObj, "maxParameters", "inlayHints"); maxParam.has_value())
    {
        cfg.features.inlayHintsMaxParameters = static_cast<size_t>(std::max(0, *maxParam));
    }
    if (auto maxLen = FindSectionInt(section, ihObj, "maxLength", "inlayHints"); maxLen.has_value())
    {
        cfg.features.inlayHintsMaxLength = static_cast<size_t>(std::max(0, *maxLen));
    }
    if (auto suppress = FindSectionBool(section, ihObj, "suppressWhenArgumentMatchesName", "inlayHints");
        suppress.has_value())
    {
        cfg.features.inlayHintsSuppressWhenArgumentMatchesName = *suppress;
    }
    if (auto tooltip = FindSectionBool(section, ihObj, "enableTooltip", "inlayHints"); tooltip.has_value())
    {
        cfg.features.inlayHintsEnableTooltip = *tooltip;
    }
    if (auto loc = FindSectionBool(section, ihObj, "enableLocation", "inlayHints"); loc.has_value())
    {
        cfg.features.inlayHintsEnableLocation = *loc;
    }
    if (auto omitted = FindSectionString(section, ihObj, "omittedDefaultArguments", "inlayHints"); omitted.has_value())
    {
        if (*omitted == "off" || *omitted == "false")
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::Off;
        }
        else if (*omitted == "declaration")
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::Declaration;
        }
        else
        {
            cfg.features.inlayHintsOmittedDefaultArguments = config::OmittedDefaultArgumentsMode::NameAndValue;
        }
    }
}

static void UpdateInlayHintKindConfig(const lsp::LSPObject& section, const lsp::LSPObject* ihObj,
                                      config::ServerConfig& cfg)
{
    if (auto params = FindSectionBool(section, ihObj, "parameters", "inlayHints"); params.has_value())
    {
        cfg.features.inlayHintsParameters = *params;
    }
    else if (auto paramNames = FindSectionBool(section, ihObj, "parameterNames", "inlayHints"); paramNames.has_value())
    {
        cfg.features.inlayHintsParameters = *paramNames;
    }
    if (auto varTypes = FindSectionBool(section, ihObj, "variableTypes", "inlayHints"); varTypes.has_value())
    {
        cfg.features.inlayHintsVariableTypes = *varTypes;
    }
    if (auto fnRetTypes = FindSectionBool(section, ihObj, "functionReturnTypes", "inlayHints"); fnRetTypes.has_value())
    {
        cfg.features.inlayHintsFunctionReturnTypes = *fnRetTypes;
    }
    if (auto enumVals = FindSectionBool(section, ihObj, "enumMemberValues", "inlayHints"); enumVals.has_value())
    {
        cfg.features.inlayHintsEnumMemberValues = *enumVals;
    }
    if (auto refMods = FindSectionBool(section, ihObj, "parameterReferenceModifiers", "inlayHints");
        refMods.has_value())
    {
        cfg.features.inlayHintsParameterReferenceModifiers = *refMods;
    }
}

static void UpdateInlayHintFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* ihObj = nullptr;
    if (const auto* ihVal = section.find("inlayHints"); ihVal && ihVal->isObject())
    {
        ihObj = &ihVal->object();
    }
    UpdateInlayHintDisplayConfig(section, ihObj, cfg);
    UpdateInlayHintKindConfig(section, ihObj, cfg);
}

static void UpdateHoverFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* hoverObj = nullptr;
    if (const auto* hoverVal = section.find("hover"); hoverVal && hoverVal->isObject())
    {
        hoverObj = &hoverVal->object();
    }
    if (auto len = FindSectionBool(section, hoverObj, "stringLiteralLength", "hover"); len.has_value())
    {
        cfg.features.hoverStringLiteralLength = *len;
    }
    if (auto pathRes = FindSectionBool(section, hoverObj, "stringLiteralPathResolution", "hover"); pathRes.has_value())
    {
        cfg.features.hoverStringLiteralPathResolution = *pathRes;
    }
    if (auto showLoc = FindSectionBool(section, hoverObj, "showFileLocation", "hover"); showLoc.has_value())
    {
        cfg.features.hoverShowFileLocation = *showLoc;
    }
    if (hoverObj)
    {
        if (const auto* paths = hoverObj->find("assetSearchPaths"); paths && paths->isArray())
        {
            cfg.features.assetSearchPaths.clear();
            for (const auto& item : paths->array())
            {
                if (item.isString() && !item.string().empty())
                {
                    cfg.features.assetSearchPaths.push_back(item.string());
                }
            }
        }
    }
}

static void UpdateCodeLensFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* clObj = nullptr;
    if (const auto* clVal = section.find("codeLens"); clVal && clVal->isObject())
    {
        clObj = &clVal->object();
    }
    if (auto enabled = FindSectionBool(section, clObj, "enabled", "codeLens"); enabled.has_value())
    {
        cfg.features.enableCodeLens = *enabled;
    }
    if (auto showFn = FindSectionBool(section, clObj, "showOnFunctions", "codeLens"); showFn.has_value())
    {
        cfg.features.codeLensShowOnFunctions = *showFn;
    }
    if (auto showCls = FindSectionBool(section, clObj, "showOnClassesAndInterfaces", "codeLens"); showCls.has_value())
    {
        cfg.features.codeLensShowOnClassesAndInterfaces = *showCls;
    }
    if (auto showProps = FindSectionBool(section, clObj, "showOnPropertiesAndVariables", "codeLens");
        showProps.has_value())
    {
        cfg.features.codeLensShowOnPropertiesAndVariables = *showProps;
    }
    if (auto showEnums = FindSectionBool(section, clObj, "showOnEnums", "codeLens"); showEnums.has_value())
    {
        cfg.features.codeLensShowOnEnums = *showEnums;
    }
    if (auto showZero = FindSectionBool(section, clObj, "showZeroReferences", "codeLens"); showZero.has_value())
    {
        cfg.features.codeLensShowZeroReferences = *showZero;
    }
    if (auto showMixin = FindSectionBool(section, clObj, "showMixinExpansion", "codeLens"); showMixin.has_value())
    {
        cfg.features.codeLensShowMixinExpansion = *showMixin;
    }
}

static void UpdateMetadataFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* metaObj = nullptr;
    if (const auto* metaVal = section.find("metadata"); metaVal && metaVal->isObject())
    {
        metaObj = &metaVal->object();
    }
    if (auto meta = FindSectionBool(section, metaObj, "enabled", "metadata"); meta.has_value())
    {
        cfg.features.enableMetadata = *meta;
    }
}

static void UpdateCompletionFeatureConfig(const lsp::LSPObject& section, config::ServerConfig& cfg)
{
    const lsp::LSPObject* compObj = nullptr;
    if (const auto* compVal = section.find("completion"); compVal && compVal->isObject())
    {
        compObj = &compVal->object();
    }
    if (auto smart = FindSectionBool(section, compObj, "smartTypeRanking", "completion"); smart.has_value())
    {
        cfg.features.completionSmartTypeRanking = *smart;
    }
    if (auto parens = FindSectionBool(section, compObj, "completeFunctionParens", "completion"); parens.has_value())
    {
        cfg.features.completionCompleteFunctionParens = *parens;
    }
    if (auto qualify = FindSectionBool(section, compObj, "qualifyEnumValues", "completion"); qualify.has_value())
    {
        cfg.features.completionQualifyEnumValues = *qualify;
    }
}

static void UpdateFeatureConfiguration(Server& server, const lsp::LSPObject& section)
{
    const lsp::LSPObject* featObj = nullptr;
    if (const auto* featVal = section.find("features"); featVal && featVal->isObject())
    {
        featObj = &featVal->object();
    }
    auto& config = server.GetMutableConfig();
    if (auto vmd = FindSectionBool(section, featObj, "enableVirtualMixinDocuments", "features"); vmd.has_value())
    {
        config.features.enableVirtualMixinDocuments = *vmd;
        server.GetMutableSymbolTable().SetVirtualMixinDocumentsEnabled(config.features.enableVirtualMixinDocuments);
        server.GetMutableSymbolTable().SetConfiguredTypes(config.types.arrayTypeName, config.types.stringTypeName);
    }
    if (auto ih = FindSectionBool(section, featObj, "inlayHints", "features"); ih.has_value())
    {
        config.features.enableInlayHints = *ih;
    }
    if (auto fmt = FindSectionBool(section, featObj, "formatting", "features"); fmt.has_value())
    {
        config.features.enableFormatting = *fmt;
    }
    if (auto otf = FindSectionBool(section, featObj, "onTypeFormatting", "features"); otf.has_value())
    {
        config.features.enableOnTypeFormatting = *otf;
    }
    if (auto cs = FindSectionBool(section, featObj, "commentSuppressions", "features"); cs.has_value())
    {
        config.features.enableCommentSuppressions = *cs;
    }

    UpdateMetadataFeatureConfig(section, config);
    UpdateCompletionFeatureConfig(section, config);
    UpdateInlayHintFeatureConfig(section, config);
    UpdateHoverFeatureConfig(section, config);
    UpdateCodeLensFeatureConfig(section, config);
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

static bool UpdatePredefinedAndProfileConfiguration(Server& server, const lsp::LSPObject& section)
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

    if (const auto* profileVal = section.find("engineProfile"); profileVal && profileVal->isString())
    {
        if (!profileVal->string().empty() && profileVal->string() != server.EngineProfile())
        {
            server.GetWorkspaceStore().SetEngineProfile(profileVal->string());
            server.LogInfo(fmt::format("Engine profile changed to '{}'; reloading predefineds", profileVal->string()));
            shouldRescan = true;
        }
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
    bool shouldReanalyse = UpdateEngineConfiguration(*this, *section);
    shouldReanalyse = UpdateDiagnosticsConfiguration(*this, *section) || shouldReanalyse;
    if (shouldReanalyse)
    {
        ReanalyseOpenDocuments();
    }
    UpdateFeatureConfiguration(*this, *section);

    bool shouldRescan = false;
    shouldRescan = UpdateModulesConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdateIncludeConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdatePredefinedAndProfileConfiguration(*this, *section) || shouldRescan;
    shouldRescan = UpdateSearchDirectoriesConfiguration(*this, *section) || shouldRescan;

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

    std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
    for (const auto& [uriStr, snapshot] : m_diagnosticsPipeline->GetCacheRef())
    {
        // Out under the client's own spelling, for the same reason PublishDiagnostics does it:
        // the key is canonical and the client matches these against its own URIs.
        const std::string outgoingUri = GetDocumentStore().GetClientUri(uriStr);

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
