#pragma once

#include "config/ServerConfig.h"

#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Supported built-in AngelScript predefined engine profiles.
 */
enum class EngineProfileKind
{
    None,
    Standard,
    SvenCoop,
    Urho3D,
    OpenXRay,
    OOTP,
    Auto
};

/**
 * @brief Parses an engine profile name string into an EngineProfileKind.
 * @param name Name string (case-insensitive, e.g. "standard", "svencoop", "urho3d", "openxray", "ootp", "none",
 *        "auto").
 * @return Resolved EngineProfileKind enum value.
 */
EngineProfileKind ParseEngineProfileKind(std::string_view name);

/**
 * @brief Whether a profile name is one this server knows, under any of its spellings.
 *        ParseEngineProfileKind answers Standard for a name it does not recognise, which is the right
 */
bool IsKnownEngineProfileName(std::string_view name);

/**
 * @brief Converts an EngineProfileKind to its canonical string identifier.
 * @param kind The engine profile kind enum.
 * @return String view of the canonical name.
 */
std::string_view EngineProfileKindToString(EngineProfileKind kind);

/**
 * @brief Returns a list of all available profile name identifiers.
 * @return Vector of string views containing available profile names.
 */
std::vector<std::string_view> GetAvailableEngineProfiles();

/**
 * @brief Retrieves the embedded AngelScript predefined stub source code for a profile.
 * @param kind The engine profile kind.
 * @return A view of a static literal valid for the whole program.
 */
std::string_view GetProfileStubText(EngineProfileKind kind);

/**
 * @brief Scheme and path every built-in profile's synthetic document URI starts with.
 */
inline constexpr std::string_view k_profileUriPrefix = "builtin:///profiles/";

/**
 * @brief Returns the synthetic URI used to index the built-in profile in the symbol table.
 * @param kind The engine profile kind.
 * @return Synthetic document URI string (e.g. "builtin:///profiles/standard.as.predefined").
 */
std::string GetProfileSyntheticUri(EngineProfileKind kind);

/**
 * @brief Analyzes workspace file names or content samples to heuristically auto-detect the engine profile.
 * @param fileNamesOrSamples Collection of workspace file names or sample text snippets.
 * @return Detected EngineProfileKind.
 */
EngineProfileKind DetectEngineProfileFromWorkspace(const std::vector<std::string>& fileNamesOrSamples);

/**
 * @brief Retrieves default engine properties associated with a built-in profile.
 * @param[in] kind Engine profile kind.
 * @return Default EngineProperties for the profile.
 */
config::EngineProperties GetDefaultEngineProperties(EngineProfileKind kind);

/**
 * @brief Configures engine profile capabilities, script bindings, and built-in type properties.
 */
struct ProfileMapping
{
    std::string_view alias;
    EngineProfileKind kind;
};
} // namespace angel_lsp::analysis
