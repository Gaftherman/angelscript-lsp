#include "config/EngineRuleParser.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include <algorithm>
#include <filesystem>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
namespace
{
std::string FindWildcardRulesInDir(const std::filesystem::path& dir)
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

std::string TryResolveSiblingRulesForStub(const std::string& stubFilePath)
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
        filename = filename.substr(0, filename.size() - k_asPredef.size());
    }
    else if (filename.ends_with(k_predef))
    {
        filename = filename.substr(0, filename.size() - k_predef.size());
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

std::string ResolveRulesPath(const std::string& configuredPath, const std::vector<std::string>& workspaceRoots,
                             const std::string& effectivePredefined, const std::vector<std::string>& fallbackStubs)
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

} // namespace

void Server::LoadWorkspaceEngineRules(const std::vector<std::string>& workspaceRoots)
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
    if (rulesPath.empty())
    {
        if (m_config.engineRules.HasRules())
        {
            LogInfo("No engine rules file found. Resetting to vanilla AngelScript safety rules.");
            m_config.engineRules = config::EngineRuleConfig{};
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

    m_config.engineRules = std::move(*parsed);
    LogInfo(fmt::format("Loaded engine rules: {} storage rule(s), {} type suggestion(s), {} scheduler rule(s)",
                        m_config.engineRules.storageRules.size(), m_config.engineRules.typeSuggestions.size(),
                        m_config.engineRules.schedulerRules.size()));

    ApplyEngineProperties(m_config.engineRules.engineProperties, m_config.engine);
}
} // namespace angel_lsp
