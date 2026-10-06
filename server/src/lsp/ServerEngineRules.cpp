#include "config/EngineRuleParser.h"
#include "lsp/Server.h"
#include "utils/IncludeResolver.h"
#include <filesystem>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{
namespace
{
std::string ResolveRulesPath(const std::string& configuredPath, const std::vector<std::string>& workspaceRoots,
                             const std::string& effectivePredefined)
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

    if (!effectivePredefined.empty())
    {
        std::filesystem::path predefPath(effectivePredefined);
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
    }

    return {};
}

void ApplyEngineProperties(const config::NativeEngineProperties& props, config::EngineProperties& engine)
{
    if (props.propertyAccessorMode.has_value())
    {
        engine.propertyAccessorMode = *props.propertyAccessorMode;
    }
    if (props.disallowGlobalVars.has_value())
    {
        engine.disallowGlobalVars = *props.disallowGlobalVars;
    }
    if (props.requireEnumScope.has_value())
    {
        engine.requireEnumScope = *props.requireEnumScope;
    }
    if (props.boolConversionMode.has_value())
    {
        engine.boolConversionMode = *props.boolConversionMode;
    }
    if (props.allowUnsafeReferences.has_value())
    {
        engine.allowUnsafeReferences = *props.allowUnsafeReferences;
    }
    if (props.allowMultilineStrings.has_value())
    {
        engine.allowMultilineStrings = *props.allowMultilineStrings;
    }
    if (props.disableIntegerDivision.has_value())
    {
        engine.disableIntegerDivision = *props.disableIntegerDivision;
    }
}
} // namespace

void Server::LoadWorkspaceEngineRules(const std::vector<std::string>& workspaceRoots)
{
    const std::string rulesPath = ResolveRulesPath(m_config.rulesFile, workspaceRoots, m_effectivePredefined);
    if (rulesPath.empty())
    {
        if (!m_config.engineRules.storageRules.empty() || !m_config.engineRules.typeSuggestions.empty() ||
            !m_config.engineRules.schedulerRules.empty())
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
