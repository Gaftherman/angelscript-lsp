#pragma once

#include "lsp/Server.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{

inline std::optional<bool> FindSectionBool(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                           std::string_view name, std::string_view prefix = "engine")
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isBoolean())
        {
            return val->boolean();
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isBoolean())
    {
        return val->boolean();
    }
    return std::nullopt;
}

inline std::optional<int> FindSectionInt(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                         std::string_view name, std::string_view prefix = "engine")
{
    if (engineObj)
    {
        if (const auto* val = engineObj->find(std::string(name)); val && val->isNumber())
        {
            return static_cast<int>(val->number());
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isNumber())
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}

inline std::optional<std::string> FindSectionString(const lsp::LSPObject& section, const lsp::LSPObject* obj,
                                                    std::string_view name, std::string_view prefix = "engine")
{
    if (obj)
    {
        if (const auto* val = obj->find(std::string(name)); val && val->isString())
        {
            return val->string();
        }
    }
    const std::string dotKey = std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(dotKey); val && val->isString())
    {
        return val->string();
    }
    const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
    if (const auto* val = section.find(fullKey); val && val->isString())
    {
        return val->string();
    }
    return std::nullopt;
}

inline std::optional<std::vector<std::string>> FindSectionStringArray(const lsp::LSPObject& section,
                                                                      const lsp::LSPObject* obj, std::string_view name,
                                                                      std::string_view prefix)
{
    const lsp::json::Value* arrayVal = nullptr;
    if (obj)
    {
        if (const auto* val = obj->find(std::string(name)); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        const std::string dotKey = std::string(prefix) + "." + std::string(name);
        if (const auto* val = section.find(dotKey); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        const std::string fullKey = "angelscript." + std::string(prefix) + "." + std::string(name);
        if (const auto* val = section.find(fullKey); val && val->isArray())
        {
            arrayVal = val;
        }
    }
    if (!arrayVal)
    {
        return std::nullopt;
    }
    std::vector<std::string> items;
    for (const auto& entry : arrayVal->array())
    {
        if (entry.isString() && !entry.string().empty())
        {
            items.push_back(entry.string());
        }
    }
    return items;
}

void UpdateFeatureConfiguration(Server& server, const lsp::LSPObject& section);
bool UpdateEngineConfiguration(Server& server, const lsp::LSPObject& section);
bool UpdateDiagnosticsConfiguration(Server& server, const lsp::LSPObject& section);

} // namespace angel_lsp
