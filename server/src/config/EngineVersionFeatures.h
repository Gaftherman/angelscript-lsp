#pragma once

#include "config/EngineRuleConfig.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::config
{

/** @brief Newest AngelScript release the server is built and tested against. */
inline constexpr std::string_view LATEST_SUPPORTED_ENGINE_VERSION = "2.39.0";

/**
 * @brief Engine property that an engine profile older than `sinceVersion` does not have.
 */
struct EnginePropertyIntroduction
{
    std::string_view key;          ///< Key in the rules file "engineProperties" object.
    std::string_view nativeName;   ///< Matching asEP_* constant.
    std::string_view sinceVersion; ///< First official release that declares the constant.
};

/**
 * @brief A rules file setting that the declared engine version does not support.
 */
struct EngineProfileIssue
{
    std::string key;          ///< Rules file key, e.g. "foreachSupport".
    std::string sinceVersion; ///< Release that introduced the setting.
    std::string version;      ///< Version declared by the engine profile.
};

/**
 * @brief Checks that text is an AngelScript release number such as 2.39.0.
 * @param[in] text Candidate version.
 * @return True for three dot separated numbers.
 */
[[nodiscard]] bool IsEngineVersionString(std::string_view text) noexcept;

/**
 * @brief Converts a release number to the AngelScript form, major * 10000 + minor * 100 + patch.
 * @param[in] text Version such as 2.39.0.
 * @return The number, or 0 when the text is not a valid version.
 */
[[nodiscard]] int EngineVersionNumber(std::string_view text) noexcept;

/**
 * @brief Lists every engine property with the release that introduced it.
 * @return Static table sorted by key.
 */
[[nodiscard]] std::span<const EnginePropertyIntroduction> EnginePropertyIntroductions() noexcept;

/**
 * @brief Finds the engine properties the rules file sets that its engine profile does not have.
 * @param[in] rules Parsed rules file with its profile and engine properties.
 * @return One issue per property newer than the declared version. Empty for a custom engine, which keeps
 *         its engineProperties as the source of truth, and for the default profile.
 */
[[nodiscard]] std::vector<EngineProfileIssue> CheckEngineProfile(const EngineRuleConfig& rules);

} // namespace angel_lsp::config
