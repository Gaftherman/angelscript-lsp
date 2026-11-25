#include "config/EngineRuleParser.h"
#include "config/EngineVersionFeatures.h"
#include "i18n/i18n.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <spdlog/fmt/fmt.h>
#include <string_view>

namespace angel_lsp
{
static std::string FindWildcardRulesInDir(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
    {
        return {};
    }
    constexpr std::string_view k_rulesSuffix = ".angelscript.rules.json";
    std::vector<std::string> matches;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (entry.is_regular_file(ec))
        {
            const std::string filename = entry.path().filename().string();
            if (filename.ends_with(k_rulesSuffix))
            {
                matches.push_back(entry.path().string());
            }
        }
    }
    if (matches.empty())
    {
        return {};
    }
    std::sort(matches.begin(), matches.end());
    return matches.front();
}

static std::string TryResolveSiblingRulesForStub(std::string_view stubFilePath)
{
    if (stubFilePath.empty())
    {
        return {};
    }
    std::error_code ec;
    std::filesystem::path predefPath(stubFilePath);
    std::string filename = predefPath.filename().string();
    constexpr std::string_view k_asPredef = ".as.predefined";
    constexpr std::string_view k_predef = ".predefined";
    if (filename.ends_with(k_asPredef))
    {
        filename.resize(filename.size() - k_asPredef.size());
    }
    else if (filename.ends_with(k_predef))
    {
        filename.resize(filename.size() - k_predef.size());
    }
    std::filesystem::path candidate = predefPath.parent_path() / (filename + ".angelscript.rules.json");
    if (std::filesystem::is_regular_file(candidate, ec))
    {
        return candidate.string();
    }
    if (std::string wildcard = FindWildcardRulesInDir(predefPath.parent_path()); !wildcard.empty())
    {
        return wildcard;
    }
    return {};
}

static std::string ResolveRulesPath(std::string_view configuredPath, std::span<const std::string> workspaceRoots,
                                    std::string_view effectivePredefined, std::span<const std::string> fallbackStubs)
{
    std::error_code ec;
    if (!configuredPath.empty())
    {
        std::filesystem::path p(configuredPath);
        if (p.is_absolute() && std::filesystem::is_regular_file(p, ec))
        {
            return p.string();
        }
        for (const auto& root : workspaceRoots)
        {
            std::filesystem::path candidate = std::filesystem::path(root) / p;
            if (std::filesystem::is_regular_file(candidate, ec))
            {
                return candidate.string();
            }
        }
    }

    for (const auto& root : workspaceRoots)
    {
        std::filesystem::path candidate = std::filesystem::path(root) / "angelscript.rules.json";
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            return candidate.string();
        }
    }

    if (std::string resolved = TryResolveSiblingRulesForStub(effectivePredefined); !resolved.empty())
    {
        return resolved;
    }

    for (const auto& stub : fallbackStubs)
    {
        if (std::string resolved = TryResolveSiblingRulesForStub(stub); !resolved.empty())
        {
            return resolved;
        }
    }

    for (const auto& root : workspaceRoots)
    {
        if (std::string wildcard = FindWildcardRulesInDir(std::filesystem::path(root)); !wildcard.empty())
        {
            return wildcard;
        }
    }

    return {};
}

void Server::SyncEffectiveNoHandleTypes()
{
    config::MergeNoHandleTypes(m_config.engineRules, m_fileNoHandleTypes, m_config.engine.noHandleTypes);
}

std::vector<std::string> Server::ParseNoHandleTypesArray(const lsp::LSPArray& arr, bool deferWarnings)
{
    std::vector<std::string> parsed;
    const auto reportInvalid = [this, deferWarnings](std::string_view invalidEntry)
    {
        if (deferWarnings)
        {
            RecordInvalidNoHandleType(invalidEntry);
        }
        else
        {
            WarnInvalidNoHandleType(invalidEntry);
        }
    };

    for (const auto& entry : arr)
    {
        if (!entry.isString())
        {
            reportInvalid("<non-string>");
            continue;
        }
        if (!config::AppendValidatedNoHandleType(entry.string(), parsed))
        {
            reportInvalid(entry.string());
        }
    }
    return parsed;
}

void Server::RecordInvalidNoHandleType(std::string_view entry)
{
    m_config.engine.invalidNoHandleTypes.emplace_back(entry);
}

void Server::WarnInvalidNoHandleType(std::string_view entry) const
{
    LogWarning(i18n::FormatMessage(
        m_i18n.get(), "warn-invalid-no-handle-type",
        "Ignoring invalid angelscript.engine.noHandleTypes entry '{}': expected a non-empty identifier or scoped name.",
        entry));
}

void Server::FlushInvalidNoHandleTypeWarnings()
{
    for (const auto& entry : m_config.engine.invalidNoHandleTypes)
    {
        WarnInvalidNoHandleType(entry);
    }
    m_config.engine.invalidNoHandleTypes.clear();
}

void Server::LoadWorkspaceEngineRules(std::span<const std::string> workspaceRoots)
{
    std::string effectivePredef;
    std::vector<std::string> fallbackStubs = m_config.activeStubFiles;
    fallbackStubs.insert(fallbackStubs.end(), m_config.predefinedFiles.begin(), m_config.predefinedFiles.end());
    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        effectivePredef = m_effectivePredefined;
        fallbackStubs.insert(fallbackStubs.end(), m_discoveredPredefined.begin(), m_discoveredPredefined.end());
    }

    const std::string rulesPath = ResolveRulesPath(m_config.rulesFile, workspaceRoots, effectivePredef, fallbackStubs);
    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        m_effectiveRulesPath = rulesPath;
    }
    if (rulesPath.empty())
    {
        const bool hadFileRules = !m_fileNoHandleTypes.empty() || !m_config.engineRules.storageRules.empty() ||
                                  !m_config.engineRules.typeSuggestions.empty() ||
                                  !m_config.engineRules.schedulerRules.empty() ||
                                  m_config.engineRules.unusedRules.HasActiveRules() ||
                                  m_config.engineRules.engineProperties.HasActiveProperties();
        m_fileNoHandleTypes.clear();
        m_config.engineRules = config::EngineRuleConfig{};
        SyncEffectiveNoHandleTypes();
        if (hadFileRules)
        {
            LogInfo("No engine rules file found. Resetting to vanilla AngelScript safety rules.");
            m_symbolTable.SetBaseClassDelegateProperties({});
        }
        return;
    }

    LogInfo(fmt::format("Loading engine rules from: {}", rulesPath));
    auto parsed = config::ParseEngineRuleConfigFile(rulesPath);
    if (!parsed)
    {
        LogError(fmt::format("Failed to parse engine rules file: {}", rulesPath));
        return;
    }

    m_fileNoHandleTypes = parsed->noHandleTypes;
    m_config.engineRules = std::move(*parsed);
    SyncEffectiveNoHandleTypes();
    m_symbolTable.SetBaseClassDelegateProperties(m_config.engineRules.unusedRules.baseClassDelegateProperties);
    LogInfo(fmt::format("Loaded engine rules: {} storage rule(s), {} type suggestion(s), {} scheduler rule(s)",
                        m_config.engineRules.storageRules.size(), m_config.engineRules.typeSuggestions.size(),
                        m_config.engineRules.schedulerRules.size()));

    ApplyEngineProperties(m_config.engineRules.engineProperties, m_config.engine);

    for (const config::EngineProfileIssue& issue : config::CheckEngineProfile(m_config.engineRules))
    {
        LogWarning(
            i18n::FormatMessage(m_i18n.get(), "warn-engine-property-too-new",
                                "engineProperties.{} needs AngelScript {} or later, but the engine profile is {}.",
                                issue.key, issue.sinceVersion, issue.version));
    }
    const config::EngineProfile& profile = m_config.engineRules.profile;
    if (profile.source != "custom" && config::EngineVersionNumber(profile.version) !=
                                          config::EngineVersionNumber(config::LATEST_SUPPORTED_ENGINE_VERSION))
    {
        LogInfo(i18n::FormatMessage(m_i18n.get(), "info-engine-profile-not-latest",
                                    "Engine profile is AngelScript {}. Version {} is fully supported; other releases "
                                    "are checked against the profile only.",
                                    profile.version, config::LATEST_SUPPORTED_ENGINE_VERSION));
    }
}
} // namespace angel_lsp
