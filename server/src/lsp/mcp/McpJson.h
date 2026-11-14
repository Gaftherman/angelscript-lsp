#pragma once

#include <lsp/json/json.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace angel_lsp::mcp::json
{

/** @brief Creates an empty JSON object value. */
inline lsp::json::Value MakeObject()
{
    return lsp::json::Value(lsp::json::Object());
}

/**
 * @brief Reads a string member of an arguments object.
 * @param[in] args Arguments value; anything other than an object counts as having no members.
 * @param[in] key Member name.
 * @param[in] required Whether a missing member is an error.
 * @return The string, or empty when absent and not required.
 * @throws std::runtime_error when required and absent, or present with another type.
 */
inline std::string GetString(const lsp::json::Value& args, std::string_view key, bool required)
{
    const lsp::json::Value* value = args.isObject() ? args.object().find(key) : nullptr;
    if (value == nullptr || value->isNull())
    {
        if (required)
        {
            throw std::runtime_error("missing argument '" + std::string(key) + "'");
        }
        return {};
    }
    if (!value->isString())
    {
        throw std::runtime_error("argument '" + std::string(key) + "' must be a string");
    }
    return value->string();
}

/**
 * @brief Reads a non-negative integer member of an arguments object.
 * @throws std::runtime_error when absent, not an integer or negative.
 */
inline int GetNonNegativeInt(const lsp::json::Value& args, std::string_view key)
{
    const lsp::json::Value* value = args.isObject() ? args.object().find(key) : nullptr;
    if (value == nullptr || !value->isInteger() || value->integer() < 0)
    {
        throw std::runtime_error("argument '" + std::string(key) + "' must be a non-negative integer");
    }
    return value->integer();
}

/** @brief Builds an LSP Position object. */
inline lsp::json::Value MakePosition(int line, int character)
{
    lsp::json::Value position = MakeObject();
    position.object()["line"] = line;
    position.object()["character"] = character;
    return position;
}

/** @brief Builds the TextDocumentIdentifier object for a URI. */
inline lsp::json::Value MakeDocumentId(std::string_view uri)
{
    lsp::json::Value id = MakeObject();
    id.object()["uri"] = std::string(uri);
    return id;
}

} // namespace angel_lsp::mcp::json
