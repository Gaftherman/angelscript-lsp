#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace angel_lsp::config
{

/**
 * @brief Rule for restricting where certain types or type hierarchies can be stored.
 */
struct StorageRule
{
    std::string id;                   ///< Unique identifier for the rule.
    std::vector<std::string> types;   ///< Exact type names to match.
    std::string inheritsFrom;         ///< Base type to match derived classes.
    std::string typeRegex;            ///< Regex pattern to match type names.
    bool handleOnly = false;          ///< If true, applies only to handles ('@').
    bool disallowInMembers = false;   ///< Prohibit storage in class/struct member variables.
    bool disallowInGlobals = false;   ///< Prohibit storage in global variables.
    bool disallowInArrays = false;    ///< Prohibit storage in array elements (array<T>).
    std::string suggestReplacement;   ///< Suggested safe replacement type (e.g. "SafeHandle").
    std::string message;              ///< Custom diagnostic message template.
    std::string severity = "warning"; ///< Diagnostic severity: "warning", "error", "info", "hint".
};

/**
 * @brief Rule for suggesting a replacement type for deprecated or sub-optimal types.
 */
struct TypeSuggestionRule
{
    std::string id;                     ///< Unique identifier for the suggestion rule.
    std::string fromType;               ///< Sub-optimal or discouraged type name (e.g. "string_t").
    std::string toType;                 ///< Recommended replacement type (e.g. "string").
    std::vector<std::string> appliesTo; ///< Scopes: "local", "member", "param", "return", or "all".
    std::string message;                ///< Custom diagnostic message template.
    std::string severity = "hint";      ///< Diagnostic severity: "hint", "info", "warning".
};

/**
 * @brief Rule for validating asynchronous or scheduled callback function invocations.
 */
struct SchedulerRule
{
    std::string id;                            ///< Unique identifier for the rule.
    std::vector<std::string> receiverTypes;    ///< Method receiver types (e.g. ["SchedulerClass"]).
    std::vector<std::string> methodNames;      ///< Method names (e.g. ["QueueTask", "ScheduleInterval"]).
    std::string disallowParamInheritsFrom;     ///< Callback parameter base type to disallow.
    std::string disallowParamRegex;            ///< Callback parameter regex to disallow.
    bool disallowParamHandleOnly = true;       ///< Disallow only handle parameters in callback.
    std::string paramSuggestReplacement;       ///< Replacement to suggest for callback parameter.
    std::string disallowArgInheritsFrom;       ///< Scheduler argument base type to disallow.
    std::string disallowArgRegex;              ///< Scheduler argument regex to disallow.
    bool disallowArgHandleOnly = true;         ///< Disallow only handle arguments passed to scheduler.
    std::string argSuggestReplacement;         ///< Replacement to suggest for scheduler argument.
    bool requireExplicitEnumConstruct = false; ///< Disallow passing raw int to enum callback param.
    bool disallowMethod = false;               ///< If true, disallow calling this method entirely.
    std::string suggestReplacement;   ///< Suggested safe replacement call (e.g. "g_EntityFuncs.DispatchKeyValue").
    std::string message;              ///< Custom diagnostic message template.
    std::string severity = "warning"; ///< Diagnostic severity: "warning", "error".
};

/**
 * @brief Native AngelScript Engine Properties (asEP_*) configured in the rules file.
 */
struct NativeEngineProperties
{
    std::optional<int> propertyAccessorMode;        ///< asEP_PROPERTY_ACCESSOR_MODE (0, 1, 2, 3).
    std::optional<bool> disallowGlobalVars;         ///< asEP_DISALLOW_GLOBAL_VARS.
    std::optional<bool> requireEnumScope;           ///< asEP_REQUIRE_ENUM_SCOPE.
    std::optional<int> boolConversionMode;          ///< asEP_BOOL_CONVERSION_MODE (0, 1).
    std::optional<bool> allowUnsafeReferences;      ///< asEP_ALLOW_UNSAFE_REFERENCES.
    std::optional<bool> allowMultilineStrings;      ///< asEP_ALLOW_MULTILINE_STRINGS.
    std::optional<bool> disableIntegerDivision;     ///< asEP_DISABLE_INTEGER_DIVISION.
    std::optional<bool> privatePropAsProtected;     ///< asEP_PRIVATE_PROP_AS_PROTECTED.
    std::optional<int> useCharacterLiterals;        ///< asEP_USE_CHARACTER_LITERALS (0, 1, 2).
    std::optional<bool> disallowValueAssignForRef;  ///< asEP_DISALLOW_VALUE_ASSIGN_FOR_REF_TYPE.
    std::optional<int> alterSyntaxNamedArgs;        ///< asEP_ALTER_SYNTAX_NAMED_ARGS (0, 1, 2).
    std::optional<bool> disallowEmptyListElements;  ///< asEP_DISALLOW_EMPTY_LIST_ELEMENTS.
    std::optional<bool> foreachSupport;             ///< asEP_FOREACH_SUPPORT.
    std::optional<bool> alwaysImplDefaultConstruct; ///< asEP_ALWAYS_IMPL_DEFAULT_CONSTRUCT.
    std::optional<bool> allowUnicodeIdentifiers;    ///< asEP_ALLOW_UNICODE_IDENTIFIERS.
    std::optional<bool> ignoreDuplicateSharedIntf;  ///< asEP_IGNORE_DUPLICATE_SHARED_INTF.
    std::optional<int> compilerWarnings;            ///< asEP_COMPILER_WARNINGS (0, 1, 2).
    std::optional<int> memberInitMode;              ///< asEP_MEMBER_INIT_MODE (0, 1).
    std::optional<int> heredocTrimMode;             ///< asEP_HEREDOC_TRIM_MODE (0, 1, 2).

    /**
     * @brief Checks if any general engine properties are configured.
     * @return True if at least one general engine property is present, false otherwise.
     */
    [[nodiscard]] bool HasGeneralProperties() const noexcept
    {
        return propertyAccessorMode.has_value() || disallowGlobalVars.has_value() || requireEnumScope.has_value() ||
               boolConversionMode.has_value() || allowUnsafeReferences.has_value() ||
               allowMultilineStrings.has_value() || disableIntegerDivision.has_value() ||
               privatePropAsProtected.has_value() || useCharacterLiterals.has_value();
    }

    /**
     * @brief Checks if any syntax or compiler properties are configured.
     * @return True if at least one syntax or compiler property is present, false otherwise.
     */
    [[nodiscard]] bool HasSyntaxOrCompilerProperties() const noexcept
    {
        return disallowValueAssignForRef.has_value() || alterSyntaxNamedArgs.has_value() ||
               disallowEmptyListElements.has_value() || foreachSupport.has_value() ||
               alwaysImplDefaultConstruct.has_value() || allowUnicodeIdentifiers.has_value() ||
               ignoreDuplicateSharedIntf.has_value() || compilerWarnings.has_value() || memberInitMode.has_value() ||
               heredocTrimMode.has_value();
    }

    /**
     * @brief Checks if any native engine properties are configured.
     * @return True if at least one engine property is present, false otherwise.
     */
    [[nodiscard]] bool HasActiveProperties() const noexcept
    {
        return HasGeneralProperties() || HasSyntaxOrCompilerProperties();
    }
};

/**
 * @brief Configuration for reflection calls passing symbol names as string literals.
 */
struct StringReflectionCallee
{
    std::string callee;               ///< Callee object or namespace (e.g. "g_CustomEntityFuncs").
    std::vector<std::string> methods; ///< Method names (e.g. ["RegisterCustomEntity"]).
    std::vector<size_t> argIndices;   ///< Argument positions containing string symbol names.
};

/**
 * @brief Transparent string hash for unordered_map lookups using string_view without heap allocations.
 */
struct ConfigStringHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
};

using StringVectorMap = std::unordered_map<std::string, std::vector<std::string>, ConfigStringHash, std::equal_to<>>;

/**
 * @brief Configuration for detecting unused symbols and exempting engine callbacks.
 */
struct UnusedSymbolRules
{
    bool enabled = false;                                 ///< Whether extended unused symbol rules are active.
    bool checkLocals = true;                              ///< Check unused local variables.
    bool checkMembers = true;                             ///< Check unused class and mixin member variables.
    bool checkGlobals = true;                             ///< Check unused global variables.
    bool checkFunctions = true;                           ///< Check unused functions.
    bool checkClasses = true;                             ///< Check unused classes.
    std::vector<std::string> ignoredGlobalFunctions;      ///< Functions called by engine (e.g. "MapInit").
    std::string ignoredGlobalFunctionRegex;               ///< Regex pattern for engine hooks (e.g. "^(On|Hook_).*").
    std::vector<std::string> ignoredBaseClasses;          ///< Entity base classes (e.g. "ScriptBaseEntity").
    std::vector<std::string> lifecycleMethods;            ///< Lifecycle callback methods (e.g. "Spawn").
    StringVectorMap baseClassLifecycleMethods;            ///< Per-base-class lifecycle methods.
    std::vector<std::string> baseClassDelegateProperties; ///< Delegate property names for dynamic base dispatch.
    StringVectorMap baseClassProxyMap;                    ///< Proxy base class mappings.
    std::vector<StringReflectionCallee> stringReflectionCallees; ///< Reflection callees taking string symbols.
    bool ignoreUnusedGlobalsWithCallInitializer =
        false; ///< Ignore unused global variables initialized with a call or constructor.
    std::vector<std::string> unusedGlobalExemptTypes; ///< Type names exempted from unused global variable warnings.
    std::vector<std::vector<std::string>>
        engineCallbackSignatures; ///< Callback function signatures called by engine (e.g. [["CBaseEntity@",
                                  ///< "CBaseEntity@", "USE_TYPE", "float"]]).

    /**
     * @brief Checks if unused symbol rules or exemptions are configured.
     * @return True if any unused symbol rules or exemptions exist.
     */
    [[nodiscard]] bool HasActiveRules() const noexcept
    {
        return enabled || !ignoredBaseClasses.empty() || !lifecycleMethods.empty() ||
               !baseClassLifecycleMethods.empty() || !baseClassDelegateProperties.empty() ||
               !baseClassProxyMap.empty() || !ignoredGlobalFunctions.empty() || !stringReflectionCallees.empty() ||
               ignoreUnusedGlobalsWithCallInitializer || !unusedGlobalExemptTypes.empty() ||
               !engineCallbackSignatures.empty();
    }
};

/**
 * @brief Complete engine rule configuration loaded from angelscript.rules.json.
 */
struct EngineRuleConfig
{
    std::string name;                                ///< Friendly name of the ruleset.
    NativeEngineProperties engineProperties;         ///< Native engine properties.
    std::vector<StorageRule> storageRules;           ///< Storage safety rules.
    std::vector<TypeSuggestionRule> typeSuggestions; ///< Type suggestion rules.
    std::vector<SchedulerRule> schedulerRules;       ///< Scheduler / callback safety rules.
    UnusedSymbolRules unusedRules;                   ///< Unused symbol detection & exemption rules.

    /**
     * @brief Checks if any rules or engine properties are active.
     * @return True if configuration contains rules or properties, false otherwise.
     */
    [[nodiscard]] bool HasRules() const noexcept
    {
        return !storageRules.empty() || !typeSuggestions.empty() || !schedulerRules.empty() ||
               unusedRules.HasActiveRules() || engineProperties.HasActiveProperties();
    }
};

} // namespace angel_lsp::config
