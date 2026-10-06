#include "config/EngineRuleParser.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace angel_lsp::config
{

namespace
{

/**
 * @brief Safely parses native engine properties from JSON.
 * @param[in] j JSON object containing engineProperties.
 * @param[out] props Output structure to populate.
 */
template <typename T> void ReadOptionalField(const nlohmann::json& j, const std::string& key, std::optional<T>& target)
{
    if (j.contains(key) && !j[key].is_null())
    {
        try
        {
            target = j[key].get<T>();
        }
        catch (const nlohmann::json::exception&)
        {
        }
    }
}

void ParseNativeProperties(const nlohmann::json& j, NativeEngineProperties& props)
{
    if (!j.contains("engineProperties") || !j["engineProperties"].is_object())
    {
        return;
    }

    const auto& ep = j["engineProperties"];
    ReadOptionalField(ep, "propertyAccessorMode", props.propertyAccessorMode);
    ReadOptionalField(ep, "disallowGlobalVars", props.disallowGlobalVars);
    ReadOptionalField(ep, "requireEnumScope", props.requireEnumScope);
    ReadOptionalField(ep, "boolConversionMode", props.boolConversionMode);
    ReadOptionalField(ep, "allowUnsafeReferences", props.allowUnsafeReferences);
    ReadOptionalField(ep, "allowMultilineStrings", props.allowMultilineStrings);
    ReadOptionalField(ep, "disableIntegerDivision", props.disableIntegerDivision);
}

/**
 * @brief Parses a single StorageRule item from JSON.
 * @param[in] item JSON object representing one rule.
 * @param[out] rule Output StorageRule structure.
 */
void ParseSingleStorageRule(const nlohmann::json& item, StorageRule& rule)
{
    rule.id = item.value("id", "");
    rule.inheritsFrom = item.value("inheritsFrom", "");
    rule.typeRegex = item.value("typeRegex", "");
    rule.handleOnly = item.value("handleOnly", false);
    rule.disallowInMembers = item.value("disallowInMembers", false);
    rule.disallowInGlobals = item.value("disallowInGlobals", false);
    rule.disallowInArrays = item.value("disallowInArrays", false);
    rule.suggestReplacement = item.value("suggestReplacement", "");
    rule.message = item.value("message", "");
    rule.severity = item.value("severity", "warning");

    if (item.contains("types") && item["types"].is_array())
    {
        for (const auto& t : item["types"])
        {
            if (t.is_string())
            {
                rule.types.push_back(t.get<std::string>());
            }
        }
    }
}

/**
 * @brief Parses storage rules array from JSON.
 * @param[in] j JSON object containing storageRules.
 * @param[out] rules Destination vector for parsed rules.
 */
void ParseStorageRules(const nlohmann::json& j, std::vector<StorageRule>& rules)
{
    if (!j.contains("storageRules") || !j["storageRules"].is_array())
    {
        return;
    }

    for (const auto& item : j["storageRules"])
    {
        if (!item.is_object())
        {
            continue;
        }
        StorageRule rule;
        ParseSingleStorageRule(item, rule);
        rules.push_back(std::move(rule));
    }
}

/**
 * @brief Parses type suggestions array from JSON.
 * @param[in] j JSON object containing typeSuggestions.
 * @param[out] suggestions Destination vector for parsed suggestions.
 */
void ParseTypeSuggestions(const nlohmann::json& j, std::vector<TypeSuggestionRule>& suggestions)
{
    if (!j.contains("typeSuggestions") || !j["typeSuggestions"].is_array())
    {
        return;
    }

    for (const auto& item : j["typeSuggestions"])
    {
        if (!item.is_object())
        {
            continue;
        }
        TypeSuggestionRule rule;
        rule.id = item.value("id", "");
        rule.fromType = item.value("fromType", "");
        rule.toType = item.value("toType", "");
        rule.message = item.value("message", "");
        rule.severity = item.value("severity", "hint");

        if (item.contains("appliesTo") && item["appliesTo"].is_array())
        {
            for (const auto& s : item["appliesTo"])
            {
                if (s.is_string())
                {
                    rule.appliesTo.push_back(s.get<std::string>());
                }
            }
        }
        suggestions.push_back(std::move(rule));
    }
}

/**
 * @brief Parses string arrays from JSON into a string vector.
 * @param[in] item JSON object.
 * @param[in] key Key name to look up.
 * @param[out] target Destination vector.
 */
void ParseStringArray(const nlohmann::json& item, const std::string& key, std::vector<std::string>& target)
{
    if (item.contains(key) && item[key].is_array())
    {
        for (const auto& entry : item[key])
        {
            if (entry.is_string())
            {
                target.push_back(entry.get<std::string>());
            }
        }
    }
}

/**
 * @brief Parses a single SchedulerRule item from JSON.
 * @param[in] item JSON object representing one scheduler rule.
 * @param[out] rule Output SchedulerRule structure.
 */
void ParseSingleSchedulerRule(const nlohmann::json& item, SchedulerRule& rule)
{
    rule.id = item.value("id", "");
    rule.disallowParamInheritsFrom = item.value("disallowParamInheritsFrom", "");
    rule.disallowParamRegex = item.value("disallowParamRegex", "");
    rule.disallowParamHandleOnly = item.value("disallowParamHandleOnly", true);
    rule.paramSuggestReplacement = item.value("paramSuggestReplacement", "");
    rule.disallowArgInheritsFrom = item.value("disallowArgInheritsFrom", "");
    rule.disallowArgRegex = item.value("disallowArgRegex", "");
    rule.disallowArgHandleOnly = item.value("disallowArgHandleOnly", true);
    rule.argSuggestReplacement = item.value("argSuggestReplacement", "");
    rule.requireExplicitEnumConstruct = item.value("requireExplicitEnumConstruct", false);
    rule.message = item.value("message", "");
    rule.severity = item.value("severity", "warning");

    ParseStringArray(item, "receiverTypes", rule.receiverTypes);
    if (item.contains("callee") && item["callee"].is_string())
    {
        rule.receiverTypes.push_back(item["callee"].get<std::string>());
    }
    ParseStringArray(item, "methodNames", rule.methodNames);
    ParseStringArray(item, "methods", rule.methodNames);
}

/**
 * @brief Parses scheduler rules array from JSON.
 * @param[in] j JSON object containing schedulerRules.
 * @param[out] rules Destination vector for parsed rules.
 */
void ParseSchedulerRules(const nlohmann::json& j, std::vector<SchedulerRule>& rules)
{
    if (!j.contains("schedulerRules") || !j["schedulerRules"].is_array())
    {
        return;
    }

    for (const auto& item : j["schedulerRules"])
    {
        if (!item.is_object())
        {
            continue;
        }
        SchedulerRule rule;
        ParseSingleSchedulerRule(item, rule);
        rules.push_back(std::move(rule));
    }
}

void ParseReflectionCallees(const nlohmann::json& obj, std::vector<StringReflectionCallee>& callees)
{
    if (!obj.contains("stringReflectionCallees") || !obj["stringReflectionCallees"].is_array())
    {
        return;
    }
    for (const auto& item : obj["stringReflectionCallees"])
    {
        if (!item.is_object())
            continue;
        StringReflectionCallee c;
        c.callee = item.value("callee", "");
        ParseStringArray(item, "methods", c.methods);
        if (item.contains("argIndices") && item["argIndices"].is_array())
        {
            for (const auto& idx : item["argIndices"])
                if (idx.is_number_unsigned())
                    c.argIndices.push_back(idx.get<size_t>());
        }
        else if (item.contains("argIndex") && item["argIndex"].is_number_unsigned())
        {
            c.argIndices.push_back(item["argIndex"].get<size_t>());
        }
        callees.push_back(std::move(c));
    }
}

void ParseUnusedSymbolRules(const nlohmann::json& j, UnusedSymbolRules& rules)
{
    if (!j.contains("unusedSymbolRules") || !j["unusedSymbolRules"].is_object())
    {
        return;
    }
    const auto& u = j["unusedSymbolRules"];
    rules.enabled = u.value("enabled", true);
    rules.checkLocals = u.value("checkLocals", true);
    rules.checkMembers = u.value("checkMembers", true);
    rules.checkGlobals = u.value("checkGlobals", true);
    rules.checkFunctions = u.value("checkFunctions", true);
    rules.checkClasses = u.value("checkClasses", true);
    rules.ignoredGlobalFunctionRegex = u.value("ignoredGlobalFunctionRegex", "");
    ParseStringArray(u, "ignoredGlobalFunctions", rules.ignoredGlobalFunctions);
    ParseStringArray(u, "ignoredBaseClasses", rules.ignoredBaseClasses);
    ParseStringArray(u, "lifecycleMethods", rules.lifecycleMethods);
    ParseReflectionCallees(u, rules.stringReflectionCallees);
}

} // namespace

std::optional<EngineRuleConfig> ParseEngineRuleConfigJson(std::string_view jsonText)
{
    try
    {
        const auto parsed = nlohmann::json::parse(jsonText);
        if (!parsed.is_object())
        {
            return std::nullopt;
        }

        EngineRuleConfig config;
        config.name = parsed.value("name", "");

        ParseNativeProperties(parsed, config.engineProperties);
        ParseStorageRules(parsed, config.storageRules);
        ParseTypeSuggestions(parsed, config.typeSuggestions);
        ParseSchedulerRules(parsed, config.schedulerRules);
        ParseUnusedSymbolRules(parsed, config.unusedRules);

        return config;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }
}

std::optional<EngineRuleConfig> ParseEngineRuleConfigFile(const std::filesystem::path& filePath)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec))
    {
        return std::nullopt;
    }

    std::ifstream file(filePath, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        return std::nullopt;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ParseEngineRuleConfigJson(ss.str());
}

} // namespace angel_lsp::config
