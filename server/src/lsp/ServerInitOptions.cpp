#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "lsp/Server.h"
#include "lsp/ServerCapabilities.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

static std::optional<bool> FindSectionBool(const lsp::LSPObject* initSection, const lsp::LSPObject* engineObj,
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
    if (const auto* val = initSection->find(dotKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = initSection->find(fullKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    return std::nullopt;
}

static std::optional<int> FindSectionInt(const lsp::LSPObject* initSection, const lsp::LSPObject* engineObj,
                                         std::string_view name)
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isNumber())
        {
            return static_cast<int>(val->number());
        }
    }
    const std::string dotKey = "engine." + std::string(name);
    if (const auto* val = initSection->find(dotKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    const std::string fullKey = "angelscript.engine." + std::string(name);
    if (const auto* val = initSection->find(fullKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}

void ExtractInitialWorkspaceRoots(WorkspaceStateStore& workspaceStore, std::mutex& runtimeConfigMutex,
                                  const lsp::requests::Initialize::Params& params)
{
    std::lock_guard<std::mutex> lock(runtimeConfigMutex);
    std::vector<std::string> roots;
    if (params.workspaceFolders.has_value() && !params.workspaceFolders.value().isNull())
    {
        for (const auto& workspace : params.workspaceFolders.value().value())
        {
            const std::string fsPath = workspace.uri.fsPath();
            if (!fsPath.empty())
            {
                roots.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(fsPath));
            }
        }
    }

    if (roots.empty())
    {
        std::string root;
        if (!params.rootUri.isNull())
        {
            root = params.rootUri.value().fsPath();
            if (root.empty())
            {
                root = angel_lsp::utils::UriToPath(params.rootUri.value().toString());
            }
        }
        if (root.empty() && params.rootPath.has_value() && !params.rootPath.value().isNull())
        {
            root = angel_lsp::utils::UriToPath(params.rootPath.value().value());
        }
        if (!root.empty())
        {
            roots.push_back(angel_lsp::utils::IncludeResolver::NormalizePath(root));
        }
    }

    workspaceStore.SetWorkspaceRoots(std::move(roots));
}

void ApplyEngineInitializationOptions(Server& server, const std::optional<lsp::LSPAny>& initOpts)
{
    if (!initOpts.has_value() || !initOpts->isObject())
    {
        return;
    }

    const lsp::LSPObject* initSection = &initOpts->object();
    if (const auto* nested = initSection->find("angelscript"); nested && nested->isObject())
    {
        initSection = &nested->object();
    }
    const lsp::LSPObject* engineObj = nullptr;
    if (const auto* e = initSection->find("engine"); e && e->isObject())
    {
        engineObj = &e->object();
    }

    auto applyBool = [&](std::string_view name, bool& target)
    {
        if (auto v = FindSectionBool(initSection, engineObj, name))
            target = *v;
    };
    auto applyInt = [&](std::string_view name, int& target)
    {
        if (auto v = FindSectionInt(initSection, engineObj, name))
            target = *v;
    };

    server.BindEngineConfiguration(applyBool, applyInt);
}

static void LogNonDefaultEngineProperties(const config::EngineProperties& engine, utils::LspLogger* logger)
{
    const config::EngineProperties defaults;
    std::string changed;

    const auto note = [&changed](bool current, bool byDefault, std::string_view name)
    {
        if (current == byDefault)
        {
            return;
        }
        if (!changed.empty())
        {
            changed += ", ";
        }
        changed += name;
        changed += current ? "=true" : "=false";
    };

    note(engine.allowUnsafeReferences, defaults.allowUnsafeReferences, "allowUnsafeReferences");
    note(engine.privatePropAsProtected, defaults.privatePropAsProtected, "privatePropAsProtected");
    note(engine.disallowGlobalVars, defaults.disallowGlobalVars, "disallowGlobalVars");
    note(engine.allowMultilineStrings, defaults.allowMultilineStrings, "allowMultilineStrings");
    note(engine.disallowValueAssignForRef, defaults.disallowValueAssignForRef, "disallowValueAssignForRef");
    note(engine.disableIntegerDivision, defaults.disableIntegerDivision, "disableIntegerDivision");
    note(engine.disallowEmptyListElements, defaults.disallowEmptyListElements, "disallowEmptyListElements");
    note(engine.foreachSupport, defaults.foreachSupport, "foreachSupport");
    note(engine.requireEnumScope, defaults.requireEnumScope, "requireEnumScope");
    note(engine.alwaysImplDefaultConstruct, defaults.alwaysImplDefaultConstruct, "alwaysImplDefaultConstruct");
    note(engine.allowUnicodeIdentifiers, defaults.allowUnicodeIdentifiers, "allowUnicodeIdentifiers");
    note(engine.ignoreDuplicateSharedIntf, defaults.ignoreDuplicateSharedIntf, "ignoreDuplicateSharedIntf");

    auto noteInt = [&](int current, int defVal, std::string_view name)
    {
        if (current == defVal)
            return;
        if (!changed.empty())
            changed += ", ";
        changed += name;
        changed += "=" + std::to_string(current);
    };

    noteInt(engine.propertyAccessorMode, defaults.propertyAccessorMode, "propertyAccessorMode");
    noteInt(engine.boolConversionMode, defaults.boolConversionMode, "boolConversionMode");
    noteInt(engine.useCharacterLiterals, defaults.useCharacterLiterals, "useCharacterLiterals");
    noteInt(engine.alterSyntaxNamedArgs, defaults.alterSyntaxNamedArgs, "alterSyntaxNamedArgs");
    noteInt(engine.compilerWarnings, defaults.compilerWarnings, "compilerWarnings");

    if (!changed.empty() && logger)
    {
        logger->LogInfo(fmt::format("Engine properties differing from the defaults: {}", changed));
    }
}

void BuildDiagnosticSeverityOverrides(DiagnosticsPipeline* pipeline, const angel_lsp::config::ServerConfig& config,
                                      utils::LspLogger* logger)
{
    ankerl::unordered_dense::map<std::string, angel_lsp::analysis::DiagnosticSeverity, angel_lsp::TransparentStringHash,
                                 std::equal_to<>>
        severities;

    for (const auto& [code, severityName] : config.diagnosticSeverities)
    {
        const auto selector = angel_lsp::analysis::NormalizeDiagnosticSelector(code);
        if (selector != code && config.diagnosticSeverities.contains(std::string(selector)))
        {
            continue;
        }
        const std::string key(selector);
        if (severityName == "error")
            severities[key] = angel_lsp::analysis::DiagnosticSeverity::Error;
        else if (severityName == "warning")
            severities[key] = angel_lsp::analysis::DiagnosticSeverity::Warning;
        else if (severityName == "information")
            severities[key] = angel_lsp::analysis::DiagnosticSeverity::Information;
        else if (severityName == "hint")
            severities[key] = angel_lsp::analysis::DiagnosticSeverity::Hint;
        else if (logger)
            logger->LogError(fmt::format("Unknown diagnostic severity '{}' for '{}'; ignored", severityName, code));
    }

    if (!severities.empty() && logger)
    {
        logger->LogInfo(fmt::format("Diagnostic severity overrides active: {}", severities.size()));
    }

    if (pipeline)
    {
        pipeline->SetDiagnosticSeverities(std::move(severities));
    }

    LogNonDefaultEngineProperties(config.engine, logger);
}

} // namespace angel_lsp
