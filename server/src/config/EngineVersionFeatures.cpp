#include "config/EngineVersionFeatures.h"

#include <array>
#include <charconv>

namespace angel_lsp::config
{

// Generated from the angelscript.h of every official release by angelscript-cli/scripts/engine-features.py.
static constexpr std::array<EnginePropertyIntroduction, 19> k_introductions = {{
    {"allowMultilineStrings", "asEP_ALLOW_MULTILINE_STRINGS", "2.20.0"},
    {"allowUnicodeIdentifiers", "asEP_ALLOW_UNICODE_IDENTIFIERS", "2.31.0"},
    {"allowUnsafeReferences", "asEP_ALLOW_UNSAFE_REFERENCES", "2.20.0"},
    {"alterSyntaxNamedArgs", "asEP_ALTER_SYNTAX_NAMED_ARGS", "2.29.1"},
    {"alwaysImplDefaultConstruct", "asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT", "2.22.1"},
    {"boolConversionMode", "asEP_BOOL_CONVERSION_MODE", "2.38.0"},
    {"compilerWarnings", "asEP_COMPILER_WARNINGS", "2.26.2"},
    {"disableIntegerDivision", "asEP_DISABLE_INTEGER_DIVISION", "2.29.2"},
    {"disallowEmptyListElements", "asEP_DISALLOW_EMPTY_LIST_ELEMENTS", "2.30.0"},
    {"disallowGlobalVars", "asEP_DISALLOW_GLOBAL_VARS", "2.22.0"},
    {"disallowValueAssignForRef", "asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE", "2.26.3"},
    {"foreachSupport", "asEP_FOREACH_SUPPORT", "2.38.0"},
    {"heredocTrimMode", "asEP_HEREDOC_TRIM_MODE", "2.31.1"},
    {"ignoreDuplicateSharedIntf", "asEP_IGNORE_DUPLICATE_SHARED_INTF", "2.36.0"},
    {"memberInitMode", "asEP_MEMBER_INIT_MODE", "2.38.0"},
    {"privatePropAsProtected", "asEP_PRIVATE_PROP_AS_PROTECTED", "2.30.0"},
    {"propertyAccessorMode", "asEP_PROPERTY_ACCESSOR_MODE", "2.20.0"},
    {"requireEnumScope", "asEP_REQUIRE_ENUM_SCOPE", "2.20.0"},
    {"useCharacterLiterals", "asEP_USE_CHARACTER_LITERALS", "2.20.0"},
}};

bool IsEngineVersionString(std::string_view text) noexcept
{
    return EngineVersionNumber(text) != 0;
}

int EngineVersionNumber(std::string_view text) noexcept
{
    int parts[3] = {0, 0, 0};
    for (int index = 0; index < 3; ++index)
    {
        const char* first = text.data();
        const char* last = first + text.size();
        const auto [end, error] = std::from_chars(first, last, parts[index]);
        if (error != std::errc{} || end == first || parts[index] < 0 || parts[index] > 99)
        {
            return 0;
        }
        text.remove_prefix(static_cast<size_t>(end - first));
        const bool lastPart = index == 2;
        if (lastPart ? !text.empty() : text.empty() || text.front() != '.')
        {
            return 0;
        }
        if (!lastPart)
        {
            text.remove_prefix(1);
        }
    }
    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

std::span<const EnginePropertyIntroduction> EnginePropertyIntroductions() noexcept
{
    return k_introductions;
}

/**
 * @brief Tells whether the rules file sets the engine property with the given key.
 * @param[in] props Parsed engine properties.
 * @param[in] key Rules file key.
 * @return True when the property is present.
 */
static bool IsPropertySet(const NativeEngineProperties& props, std::string_view key)
{
    const std::array<std::pair<std::string_view, bool>, 19> state = {{
        {"allowMultilineStrings", props.allowMultilineStrings.has_value()},
        {"allowUnicodeIdentifiers", props.allowUnicodeIdentifiers.has_value()},
        {"allowUnsafeReferences", props.allowUnsafeReferences.has_value()},
        {"alterSyntaxNamedArgs", props.alterSyntaxNamedArgs.has_value()},
        {"alwaysImplDefaultConstruct", props.alwaysImplDefaultConstruct.has_value()},
        {"boolConversionMode", props.boolConversionMode.has_value()},
        {"compilerWarnings", props.compilerWarnings.has_value()},
        {"disableIntegerDivision", props.disableIntegerDivision.has_value()},
        {"disallowEmptyListElements", props.disallowEmptyListElements.has_value()},
        {"disallowGlobalVars", props.disallowGlobalVars.has_value()},
        {"disallowValueAssignForRef", props.disallowValueAssignForRef.has_value()},
        {"foreachSupport", props.foreachSupport.has_value()},
        {"heredocTrimMode", props.heredocTrimMode.has_value()},
        {"ignoreDuplicateSharedIntf", props.ignoreDuplicateSharedIntf.has_value()},
        {"memberInitMode", props.memberInitMode.has_value()},
        {"privatePropAsProtected", props.privatePropAsProtected.has_value()},
        {"propertyAccessorMode", props.propertyAccessorMode.has_value()},
        {"requireEnumScope", props.requireEnumScope.has_value()},
        {"useCharacterLiterals", props.useCharacterLiterals.has_value()},
    }};
    for (const auto& [name, set] : state)
    {
        if (name == key)
        {
            return set;
        }
    }
    return false;
}

std::vector<EngineProfileIssue> CheckEngineProfile(const EngineRuleConfig& rules)
{
    std::vector<EngineProfileIssue> issues;
    if (rules.profile.source == "custom")
    {
        return issues;
    }
    const int declared = EngineVersionNumber(rules.profile.version);
    for (const EnginePropertyIntroduction& entry : k_introductions)
    {
        if (declared < EngineVersionNumber(entry.sinceVersion) && IsPropertySet(rules.engineProperties, entry.key))
        {
            issues.push_back({std::string(entry.key), std::string(entry.sinceVersion), rules.profile.version});
        }
    }
    return issues;
}

} // namespace angel_lsp::config
