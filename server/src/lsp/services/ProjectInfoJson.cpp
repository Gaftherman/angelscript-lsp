#include "lsp/services/ProjectInfoJson.h"

#include "lsp/mcp/McpJson.h"

#include <filesystem>
#include <optional>
#include <utility>

namespace angel_lsp
{

std::string RelativeToRoot(std::string_view root, std::string_view path)
{
    const std::filesystem::path target(path);
    if (root.empty() || path.empty())
    {
        return target.generic_string();
    }
    const std::filesystem::path relative = target.lexically_relative(std::filesystem::path(root));
    if (relative.empty() || *relative.begin() == "..")
    {
        return target.generic_string();
    }
    return relative.generic_string();
}

static lsp::json::Value StringArray(std::span<const std::string> values)
{
    lsp::json::Array array;
    for (const std::string& value : values)
    {
        array.push_back(lsp::json::Value(std::string(value)));
    }
    return lsp::json::Value(std::move(array));
}

static lsp::json::Value PathArray(std::string_view root, std::span<const std::string> paths)
{
    lsp::json::Array array;
    for (const std::string& path : paths)
    {
        array.push_back(lsp::json::Value(RelativeToRoot(root, path)));
    }
    return lsp::json::Value(std::move(array));
}

static lsp::json::Value OptionalPath(std::string_view root, std::string_view path)
{
    if (path.empty())
    {
        return lsp::json::Value(lsp::json::Null{});
    }
    return lsp::json::Value(RelativeToRoot(root, path));
}

static lsp::json::Value ModulesJson(std::string_view root,
                                    std::span<const config::ServerConfig::ModuleDefinition> modules)
{
    lsp::json::Array array;
    for (const config::ServerConfig::ModuleDefinition& module : modules)
    {
        lsp::json::Value entry = mcp::json::MakeObject();
        entry.object()["name"] = std::string(module.name);
        entry.object()["entry"] = OptionalPath(root, module.entry);
        entry.object()["folder"] = OptionalPath(root, module.folder);
        array.push_back(std::move(entry));
    }
    return lsp::json::Value(std::move(array));
}

lsp::json::Value BuildWorkspaceInfoJson(const WorkspaceInfoView& view)
{
    lsp::json::Value info = mcp::json::MakeObject();
    info.object()["root"] = OptionalPath({}, view.root);
    info.object()["moduleEntryPoint"] = OptionalPath(view.root, view.moduleEntryPoint);
    info.object()["activePredefined"] = PathArray(view.root, view.activePredefined);
    info.object()["rulesFile"] = OptionalPath(view.root, view.rulesFile);
    info.object()["includeRoots"] = PathArray(view.root, view.includeRoots);
    info.object()["modules"] = ModulesJson(view.root, view.modules);
    return info;
}

lsp::json::Value BuildModuleGraphJson(const ModuleGraphView& view)
{
    lsp::json::Value graph = mcp::json::MakeObject();
    graph.object()["entry"] = OptionalPath(view.root, view.entry);
    graph.object()["files"] = PathArray(view.root, view.files);
    graph.object()["truncated"] = view.truncated;
    graph.object()["modules"] = ModulesJson(view.root, view.modules);
    return graph;
}

template <class T> static void SetIfPresent(lsp::json::Value& object, const char* key, const std::optional<T>& value)
{
    if (value.has_value())
    {
        object.object()[key] = *value;
    }
}

static lsp::json::Value EnginePropertiesJson(const config::NativeEngineProperties& properties)
{
    lsp::json::Value json = mcp::json::MakeObject();
    SetIfPresent(json, "propertyAccessorMode", properties.propertyAccessorMode);
    SetIfPresent(json, "disallowGlobalVars", properties.disallowGlobalVars);
    SetIfPresent(json, "requireEnumScope", properties.requireEnumScope);
    SetIfPresent(json, "boolConversionMode", properties.boolConversionMode);
    SetIfPresent(json, "allowUnsafeReferences", properties.allowUnsafeReferences);
    SetIfPresent(json, "allowMultilineStrings", properties.allowMultilineStrings);
    SetIfPresent(json, "disableIntegerDivision", properties.disableIntegerDivision);
    SetIfPresent(json, "privatePropAsProtected", properties.privatePropAsProtected);
    SetIfPresent(json, "useCharacterLiterals", properties.useCharacterLiterals);
    SetIfPresent(json, "disallowValueAssignForRef", properties.disallowValueAssignForRef);
    SetIfPresent(json, "alterSyntaxNamedArgs", properties.alterSyntaxNamedArgs);
    SetIfPresent(json, "disallowEmptyListElements", properties.disallowEmptyListElements);
    SetIfPresent(json, "foreachSupport", properties.foreachSupport);
    SetIfPresent(json, "alwaysImplDefaultConstruct", properties.alwaysImplDefaultConstruct);
    SetIfPresent(json, "allowUnicodeIdentifiers", properties.allowUnicodeIdentifiers);
    SetIfPresent(json, "ignoreDuplicateSharedIntf", properties.ignoreDuplicateSharedIntf);
    SetIfPresent(json, "compilerWarnings", properties.compilerWarnings);
    SetIfPresent(json, "memberInitMode", properties.memberInitMode);
    SetIfPresent(json, "heredocTrimMode", properties.heredocTrimMode);
    return json;
}

static lsp::json::Value StorageRulesJson(std::span<const config::StorageRule> rules)
{
    lsp::json::Array array;
    for (const config::StorageRule& rule : rules)
    {
        lsp::json::Value json = mcp::json::MakeObject();
        json.object()["id"] = std::string(rule.id);
        json.object()["types"] = StringArray(rule.types);
        json.object()["inheritsFrom"] = std::string(rule.inheritsFrom);
        json.object()["typeRegex"] = std::string(rule.typeRegex);
        json.object()["handleOnly"] = rule.handleOnly;
        json.object()["disallowInMembers"] = rule.disallowInMembers;
        json.object()["disallowInGlobals"] = rule.disallowInGlobals;
        json.object()["disallowInArrays"] = rule.disallowInArrays;
        json.object()["suggestReplacement"] = std::string(rule.suggestReplacement);
        json.object()["message"] = std::string(rule.message);
        json.object()["severity"] = std::string(rule.severity);
        array.push_back(std::move(json));
    }
    return lsp::json::Value(std::move(array));
}

static lsp::json::Value TypeSuggestionsJson(std::span<const config::TypeSuggestionRule> rules)
{
    lsp::json::Array array;
    for (const config::TypeSuggestionRule& rule : rules)
    {
        lsp::json::Value json = mcp::json::MakeObject();
        json.object()["id"] = std::string(rule.id);
        json.object()["fromType"] = std::string(rule.fromType);
        json.object()["toType"] = std::string(rule.toType);
        json.object()["appliesTo"] = StringArray(rule.appliesTo);
        json.object()["message"] = std::string(rule.message);
        json.object()["severity"] = std::string(rule.severity);
        array.push_back(std::move(json));
    }
    return lsp::json::Value(std::move(array));
}

static lsp::json::Value CallRulesJson(std::span<const config::SchedulerRule> rules)
{
    lsp::json::Array array;
    for (const config::SchedulerRule& rule : rules)
    {
        lsp::json::Value json = mcp::json::MakeObject();
        json.object()["id"] = std::string(rule.id);
        json.object()["receiverTypes"] = StringArray(rule.receiverTypes);
        json.object()["methods"] = StringArray(rule.methodNames);
        json.object()["disallowParamInheritsFrom"] = std::string(rule.disallowParamInheritsFrom);
        json.object()["disallowParamRegex"] = std::string(rule.disallowParamRegex);
        json.object()["disallowParamHandleOnly"] = rule.disallowParamHandleOnly;
        json.object()["paramSuggestReplacement"] = std::string(rule.paramSuggestReplacement);
        json.object()["disallowArgInheritsFrom"] = std::string(rule.disallowArgInheritsFrom);
        json.object()["disallowArgRegex"] = std::string(rule.disallowArgRegex);
        json.object()["disallowArgHandleOnly"] = rule.disallowArgHandleOnly;
        json.object()["argSuggestReplacement"] = std::string(rule.argSuggestReplacement);
        json.object()["requireExplicitEnumConstruct"] = rule.requireExplicitEnumConstruct;
        json.object()["disallowMethod"] = rule.disallowMethod;
        json.object()["suggestReplacement"] = std::string(rule.suggestReplacement);
        json.object()["message"] = std::string(rule.message);
        json.object()["severity"] = std::string(rule.severity);
        array.push_back(std::move(json));
    }
    return lsp::json::Value(std::move(array));
}

static lsp::json::Value UnusedSymbolRulesJson(const config::UnusedSymbolRules& rules)
{
    lsp::json::Value json = mcp::json::MakeObject();
    json.object()["enabled"] = rules.enabled;
    json.object()["ignoredGlobalFunctions"] = StringArray(rules.ignoredGlobalFunctions);
    json.object()["ignoredGlobalFunctionRegex"] = std::string(rules.ignoredGlobalFunctionRegex);
    json.object()["ignoredBaseClasses"] = StringArray(rules.ignoredBaseClasses);
    json.object()["lifecycleMethods"] = StringArray(rules.lifecycleMethods);
    json.object()["unusedGlobalExemptTypes"] = StringArray(rules.unusedGlobalExemptTypes);

    lsp::json::Array reflection;
    for (const config::StringReflectionCallee& callee : rules.stringReflectionCallees)
    {
        lsp::json::Value entry = mcp::json::MakeObject();
        entry.object()["callee"] = std::string(callee.callee);
        entry.object()["methods"] = StringArray(callee.methods);
        lsp::json::Array indices;
        for (const size_t index : callee.argIndices)
        {
            indices.push_back(lsp::json::Value(static_cast<int>(index)));
        }
        entry.object()["argIndices"] = lsp::json::Value(std::move(indices));
        reflection.push_back(std::move(entry));
    }
    json.object()["stringReflectionCallees"] = lsp::json::Value(std::move(reflection));
    return json;
}

lsp::json::Value BuildEngineRulesJson(const config::EngineRuleConfig& rules, std::span<const std::string> noHandleTypes,
                                      std::string_view rulesFile)
{
    lsp::json::Value json = mcp::json::MakeObject();
    json.object()["name"] = std::string(rules.name);
    json.object()["rulesFile"] =
        rulesFile.empty() ? lsp::json::Value(lsp::json::Null{}) : lsp::json::Value(std::string(rulesFile));
    json.object()["engineProperties"] = EnginePropertiesJson(rules.engineProperties);
    json.object()["noHandleTypes"] = StringArray(noHandleTypes);
    json.object()["storageRules"] = StorageRulesJson(rules.storageRules);
    json.object()["typeSuggestions"] = TypeSuggestionsJson(rules.typeSuggestions);
    json.object()["callRules"] = CallRulesJson(rules.schedulerRules);
    json.object()["unusedSymbolRules"] = UnusedSymbolRulesJson(rules.unusedRules);
    return json;
}

} // namespace angel_lsp
