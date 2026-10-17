#pragma once

#include "config/EngineRuleConfig.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace angel_lsp::config
{

/**
 * @brief Parses an EngineRuleConfig from a JSON string.
 * @param[in] jsonText UTF-8 JSON text representation of the configuration.
 * @return Parsed EngineRuleConfig if valid, std::nullopt on parse error.
 */
std::optional<EngineRuleConfig> ParseEngineRuleConfigJson(std::string_view jsonText);

/**
 * @brief Reads and parses an EngineRuleConfig from a disk file path.
 * @param[in] filePath Path to the JSON rules configuration file.
 * @return Parsed EngineRuleConfig if found and valid, std::nullopt otherwise.
 */
std::optional<EngineRuleConfig> ParseEngineRuleConfigFile(const std::filesystem::path& filePath);

} // namespace angel_lsp::config
