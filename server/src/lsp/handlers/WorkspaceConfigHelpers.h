#pragma once

#include "lsp/Server.h"

#include <optional>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp
{

/**
 * @brief Looks up a settings key built as `<prefix>.<name>` or `angelscript.<prefix>.<name>` without heap use.
 * @param[in] section Settings object searched by the dotted keys.
 * @param[in] pattern Compile-time checked fmt pattern taking `prefix` then `name`.
 * @param[in] prefix Settings group, for example `engine`.
 * @param[in] name Setting name inside the group.
 * @return The value stored under the composed key, or nullptr when absent.
 * @note Keys up to 256 bytes use a stack buffer; longer keys fall back to one `std::string`.
 */
inline const lsp::json::Value* FindComposedKey(const lsp::LSPObject& section,
                                               fmt::format_string<std::string_view, std::string_view> pattern,
                                               std::string_view prefix, std::string_view name)
{
    char buf[256];
    const auto args = fmt::make_format_args(prefix, name);
    const auto res = fmt::vformat_to_n(buf, sizeof(buf), pattern, args);
    if (res.size <= sizeof(buf))
    {
        return section.find(std::string_view(buf, res.size));
    }
    return section.find(fmt::vformat(pattern, args));
}

/**
 * @brief Finds a setting of a given JSON type, trying the group object first and then the dotted keys.
 * @param[in] section Settings object that holds the dotted keys.
 * @param[in] obj Optional group object (for example the `engine` object); may be null.
 * @param[in] name Setting name inside the group.
 * @param[in] prefix Settings group used to compose the dotted keys.
 * @param[in] hasType Predicate that accepts only values of the wanted JSON type.
 * @return The first value that satisfies `hasType`, or nullptr; a value of another type falls through.
 */
template <typename TypePredicate>
inline const lsp::json::Value* FindSectionValue(const lsp::LSPObject& section, const lsp::LSPObject* obj,
                                                std::string_view name, std::string_view prefix, TypePredicate hasType)
{
    if (obj)
    {
        if (const auto* val = obj->find(name); val && hasType(*val))
        {
            return val;
        }
    }
    if (const auto* val = FindComposedKey(section, "{}.{}", prefix, name); val && hasType(*val))
    {
        return val;
    }
    if (const auto* val = FindComposedKey(section, "angelscript.{}.{}", prefix, name); val && hasType(*val))
    {
        return val;
    }
    return nullptr;
}

inline std::optional<bool> FindSectionBool(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                           std::string_view name, std::string_view prefix = "engine")
{
    if (const auto* val =
            FindSectionValue(section, engineObj, name, prefix, [](const auto& v) { return v.isBoolean(); }))
    {
        return val->boolean();
    }
    return std::nullopt;
}

inline std::optional<int> FindSectionInt(const lsp::LSPObject& section, const lsp::LSPObject* engineObj,
                                         std::string_view name, std::string_view prefix = "engine")
{
    if (const auto* val =
            FindSectionValue(section, engineObj, name, prefix, [](const auto& v) { return v.isNumber(); }))
    {
        return static_cast<int>(val->number());
    }
    return std::nullopt;
}

inline std::optional<std::string> FindSectionString(const lsp::LSPObject& section, const lsp::LSPObject* obj,
                                                    std::string_view name, std::string_view prefix = "engine")
{
    if (const auto* val = FindSectionValue(section, obj, name, prefix, [](const auto& v) { return v.isString(); }))
    {
        return val->string();
    }
    return std::nullopt;
}

inline std::optional<std::vector<std::string>> FindSectionStringArray(const lsp::LSPObject& section,
                                                                      const lsp::LSPObject* obj, std::string_view name,
                                                                      std::string_view prefix)
{
    const auto* arrayVal = FindSectionValue(section, obj, name, prefix, [](const auto& v) { return v.isArray(); });
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
