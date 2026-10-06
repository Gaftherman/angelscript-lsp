#pragma once

#include <optional>
#include <string>
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
    std::string suggestReplacement;   ///< Suggested safe replacement type (e.g. "EHandle").
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
    std::vector<std::string> receiverTypes;    ///< Method receiver types (e.g. ["CScheduler"]).
    std::vector<std::string> methodNames;      ///< Method names (e.g. ["SetTimeout", "SetInterval"]).
    std::string disallowParamInheritsFrom;     ///< Callback parameter base type to disallow.
    std::string disallowParamRegex;            ///< Callback parameter regex to disallow.
    bool disallowParamHandleOnly = true;       ///< Disallow only handle parameters in callback.
    std::string paramSuggestReplacement;       ///< Replacement to suggest for callback parameter.
    std::string disallowArgInheritsFrom;       ///< Scheduler argument base type to disallow.
    std::string disallowArgRegex;              ///< Scheduler argument regex to disallow.
    bool disallowArgHandleOnly = true;         ///< Disallow only handle arguments passed to scheduler.
    std::string argSuggestReplacement;         ///< Replacement to suggest for scheduler argument.
    bool requireExplicitEnumConstruct = false; ///< Disallow passing raw int to enum callback param.
    std::string message;                       ///< Custom diagnostic message template.
    std::string severity = "warning";          ///< Diagnostic severity: "warning", "error".
};

/**
 * @brief Native AngelScript Engine Properties (asEP_*) configured in the rules file.
 */
struct NativeEngineProperties
{
    std::optional<int> propertyAccessorMode;    ///< asEP_PROPERTY_ACCESSOR_MODE (0, 1, 2, 3).
    std::optional<bool> disallowGlobalVars;     ///< asEP_DISALLOW_GLOBAL_VARS.
    std::optional<bool> requireEnumScope;       ///< asEP_REQUIRE_ENUM_SCOPE.
    std::optional<int> boolConversionMode;      ///< asEP_BOOL_CONVERSION_MODE (0, 1).
    std::optional<bool> allowUnsafeReferences;  ///< asEP_ALLOW_UNSAFE_REFERENCES.
    std::optional<bool> allowMultilineStrings;  ///< asEP_ALLOW_MULTILINE_STRINGS.
    std::optional<bool> disableIntegerDivision; ///< asEP_DISABLE_INTEGER_DIVISION.
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
 * @brief Configuration for detecting unused symbols and exempting engine callbacks.
 */
struct UnusedSymbolRules
{
    bool enabled = false;                            ///< Whether extended unused symbol rules are active.
    bool checkLocals = true;                         ///< Check unused local variables.
    bool checkMembers = true;                        ///< Check unused class and mixin member variables.
    bool checkGlobals = true;                        ///< Check unused global variables.
    bool checkFunctions = true;                      ///< Check unused functions.
    bool checkClasses = true;                        ///< Check unused classes.
    std::vector<std::string> ignoredGlobalFunctions; ///< Functions called by engine (e.g. "MapInit").
    std::string ignoredGlobalFunctionRegex;          ///< Regex pattern for engine hooks (e.g. "^(On|Hook_).*").
    std::vector<std::string> ignoredBaseClasses;     ///< Entity base classes (e.g. "ScriptBaseEntity").
    std::vector<std::string> lifecycleMethods;       ///< Lifecycle callback methods (e.g. "Spawn").
    std::vector<StringReflectionCallee> stringReflectionCallees; ///< Reflection callees taking string symbols.

    /**
     * @brief Checks if unused symbol rules or exemptions are configured.
     * @return True if any unused symbol rules or exemptions exist.
     */
    [[nodiscard]] bool HasActiveRules() const noexcept
    {
        return enabled || !ignoredBaseClasses.empty() || !lifecycleMethods.empty() || !ignoredGlobalFunctions.empty() ||
               !stringReflectionCallees.empty();
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
               unusedRules.HasActiveRules();
    }
};

} // namespace angel_lsp::config
