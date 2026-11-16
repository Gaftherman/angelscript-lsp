#pragma once

#include "config/EngineRuleConfig.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace angel_lsp::config
{

/**
 * @brief Validates and normalizes a type name configured for `noHandleTypes`.
 *
 * Accepts `string` or `Ns::Vec3`, with an optional leading `::` that is stripped.
 *
 * @param[in] rawTypeName Raw type name from JSON rules, CLI flags, or LSP settings.
 * @return Normalized view (without leading `::`) when valid, or `std::nullopt` if invalid.
 */
[[nodiscard]] std::optional<std::string_view> ValidateNoHandleTypeName(std::string_view rawTypeName) noexcept;

/**
 * @brief Merges `noHandleTypes` from the rules file and plugin setting into `effective` (set union).
 * @param[in,out] effective Effective engine rule configuration whose `noHandleTypes` set is rebuilt.
 * @param[in] fromRulesFile Type names parsed from `angelscript.rules.json`.
 * @param[in] fromSettings Type names configured via CLI or `angelscript.engine.noHandleTypes`.
 */
void MergeNoHandleTypes(EngineRuleConfig& effective, const TransparentStringSet& fromRulesFile,
                        std::span<const std::string> fromSettings);

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
