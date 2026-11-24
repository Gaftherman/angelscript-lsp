#pragma once

#include "config/EngineRuleConfig.h"
#include "config/ServerConfig.h"

#include <lsp/json/json.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{

/**
 * @brief Workspace facts an agent needs before editing: where things are and which settings apply.
 */
struct WorkspaceInfoView
{
    /// First workspace root, or empty when none is open.
    std::string root;
    /// Absolute module entry-point script, or empty when none is configured.
    std::string moduleEntryPoint;
    /// Paths of the predefined stubs currently loaded.
    std::vector<std::string> activePredefined;
    /// Absolute path of the engine rules file in effect, or empty when none was found.
    std::string rulesFile;
    /// Directories that `#include` paths resolve against.
    std::vector<std::string> includeRoots;
    /// Modules the workspace builds.
    std::span<const config::ServerConfig::ModuleDefinition> modules;
};

/**
 * @brief The files of one module, as reached from its entry point.
 */
struct ModuleGraphView
{
    /// First workspace root, used to print relative paths.
    std::string root;
    /// Absolute entry-point script the files were reached from, or empty.
    std::string entry;
    /// Absolute paths of the reachable files, in a stable order.
    std::vector<std::string> files;
    /// Whether the list was cut at the caller's limit.
    bool truncated = false;
    /// Modules the workspace builds.
    std::span<const config::ServerConfig::ModuleDefinition> modules;
};

/**
 * @brief Prints a path relative to a root when it lies inside it.
 * @param[in] root Directory to measure against; empty keeps the path as given.
 * @param[in] path Absolute or relative path.
 * @return Forward-slash path, relative to `root` when inside it.
 */
[[nodiscard]] std::string RelativeToRoot(std::string_view root, std::string_view path);

/**
 * @brief Builds the workspace_info result.
 * @param[in] view Facts gathered from the running server.
 * @return JSON object with root, moduleEntryPoint, activePredefined, rulesFile, includeRoots and modules.
 */
[[nodiscard]] lsp::json::Value BuildWorkspaceInfoJson(const WorkspaceInfoView& view);

/**
 * @brief Builds the engine_rules result.
 * @param[in] rules Rules loaded from the engine rules file.
 * @param[in] noHandleTypes Types without handles after merging the file and the settings.
 * @param[in] rulesFile Path of the file the rules came from, or empty.
 * @return JSON object with the engine properties, safety rules and unused-symbol exemptions.
 */
[[nodiscard]] lsp::json::Value BuildEngineRulesJson(const config::EngineRuleConfig& rules,
                                                    std::span<const std::string> noHandleTypes,
                                                    std::string_view rulesFile);

/**
 * @brief Builds the module_graph result.
 * @param[in] view Entry point, reachable files and configured modules.
 * @return JSON object with entry, files, truncated and modules.
 */
[[nodiscard]] lsp::json::Value BuildModuleGraphJson(const ModuleGraphView& view);

} // namespace angel_lsp
