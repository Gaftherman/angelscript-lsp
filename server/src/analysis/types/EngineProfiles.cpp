#include "analysis/types/EngineProfiles.h"

#include "analysis/types/EngineProfilesInternal.h"
#include "utils/Utils.h"

#include <algorithm>
#include <optional>

namespace angel_lsp::analysis
{
EngineProfileKind ParseEngineProfileKind(std::string_view name)
{
    static constexpr ProfileMapping kMappings[] = {
        {"none", EngineProfileKind::None},          {"standard", EngineProfileKind::Standard},
        {"std", EngineProfileKind::Standard},       {"default", EngineProfileKind::Standard},
        {"svencoop", EngineProfileKind::SvenCoop},  {"sven", EngineProfileKind::SvenCoop},
        {"sven_coop", EngineProfileKind::SvenCoop}, {"svenco-op", EngineProfileKind::SvenCoop},
        {"urho3d", EngineProfileKind::Urho3D},      {"urho", EngineProfileKind::Urho3D},
        {"atomic", EngineProfileKind::Urho3D},      {"openxray", EngineProfileKind::OpenXRay},
        {"xray", EngineProfileKind::OpenXRay},      {"stalker", EngineProfileKind::OpenXRay},
        {"ootp", EngineProfileKind::OOTP},          {"ootpbaseball", EngineProfileKind::OOTP},
        {"auto", EngineProfileKind::Auto},          {"detect", EngineProfileKind::Auto},
    };

    for (const auto& entry : kMappings)
    {
        if (utils::CaseInsensitiveEquals(name, entry.alias))
        {
            return entry.kind;
        }
    }

    // Unrecognised. Standard is the right thing to LOAD - a workspace with a typo in its
    // profile name is better off with the standard library than with nothing - but the caller
    // has to be able to say so, which is what IsKnownEngineProfileName is for. Returning
    // Standard silently meant a mistyped `svencop` gave you a workspace with no host types and
    // nothing on screen explaining why.
    return EngineProfileKind::Standard;
}

bool IsKnownEngineProfileName(std::string_view name)
{
    static constexpr std::string_view k_known[] = {
        "none", "standard", "std",      "default", "svencoop", "sven", "sven_coop",    "svenco-op", "urho3d",
        "urho", "atomic",   "openxray", "xray",    "stalker",  "ootp", "ootpbaseball", "auto",      "detect",
    };

    for (const std::string_view candidate : k_known)
    {
        if (utils::CaseInsensitiveEquals(name, candidate))
            return true;
    }
    return false;
}

/**
 * @brief Converts an EngineProfileKind enum value to its canonical string representation.
 * @param[in] kind Engine profile enumeration value.
 * @return String view literal corresponding to the engine profile identifier.
 */
std::string_view EngineProfileKindToString(EngineProfileKind kind)
{
    switch (kind)
    {
    case EngineProfileKind::None:
        return "none";
    case EngineProfileKind::Standard:
        return "standard";
    case EngineProfileKind::SvenCoop:
        return "svencoop";
    case EngineProfileKind::Urho3D:
        return "urho3d";
    case EngineProfileKind::OpenXRay:
        return "openxray";
    case EngineProfileKind::OOTP:
        return "ootp";
    case EngineProfileKind::Auto:
        return "auto";
    }
    return "standard";
}

std::vector<std::string_view> GetAvailableEngineProfiles()
{
    return {"none", "standard", "svencoop", "urho3d", "openxray", "ootp", "auto"};
}

/**
 * @brief Checks if a string contains any of the provided keywords.
 * @param[in] text Target string to search within.
 * @param[in] keywords List of candidate substrings.
 * @return True if at least one keyword is found.
 */
static bool MatchesAnyKeyword(std::string_view text, std::initializer_list<std::string_view> keywords) noexcept
{
    return std::any_of(keywords.begin(), keywords.end(),
                       [text](std::string_view kw) { return utils::CaseInsensitiveContains(text, kw); });
}

static std::optional<EngineProfileKind> MatchProfileSample(std::string_view sample)
{
    if (MatchesAnyKeyword(sample, {"svencoop", "sven", "cbaseplayer", "g_playerfuncs", "g_enginefuncs"}))
    {
        return EngineProfileKind::SvenCoop;
    }
    if (MatchesAnyKeyword(sample, {"urho3d", "urho", "atomic", "subscribetoevent", "variantmap"}))
    {
        return EngineProfileKind::Urho3D;
    }
    if (MatchesAnyKeyword(sample, {"openxray", "xray", "stalker", "ini_file", "alife_simulator"}))
    {
        return EngineProfileKind::OpenXRay;
    }
    if (MatchesAnyKeyword(sample, {"ootp", "ootpplayer", "ootpteam"}))
    {
        return EngineProfileKind::OOTP;
    }
    return std::nullopt;
}

std::string_view GetProfileStubText(EngineProfileKind kind)
{
    return profiles::RawProfileStub(kind);
}

std::string GetProfileSyntheticUri(EngineProfileKind kind)
{
    return std::string(k_profileUriPrefix) + std::string(EngineProfileKindToString(kind)) + ".as.predefined";
}

EngineProfileKind DetectEngineProfileFromWorkspace(const std::vector<std::string>& fileNamesOrSamples)
{
    for (const auto& item : fileNamesOrSamples)
    {
        if (const auto matched = MatchProfileSample(item))
        {
            return *matched;
        }
    }

    return EngineProfileKind::Standard;
}

config::EngineProperties GetDefaultEngineProperties(EngineProfileKind kind)
{
    config::EngineProperties props;
    switch (kind)
    {
    case EngineProfileKind::SvenCoop:
        props.propertyAccessorMode = 2;
        props.allowUnsafeReferences = false;
        props.disallowGlobalVars = false;
        props.boolConversionMode = 0;
        props.foreachSupport = true;
        break;
    default:
        break;
    }
    return props;
}
} // namespace angel_lsp::analysis
