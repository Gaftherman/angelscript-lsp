#include "lsp/mcp/McpJson.h"
#include "lsp/mcp/McpTools.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <lsp/uri.h>

namespace angel_lsp::mcp
{

/// Extra pulls attempted when the server reports diagnostics are still being computed.
static constexpr int k_diagnosticRetries = 3;

/// Completion items returned when the caller gives no limit.
static constexpr int k_defaultCompletionLimit = 50;

/** @brief Wraps one text document item into the didOpen params. */
static lsp::json::Value MakeOpenParams(std::string_view uri, int version, std::string text)
{
    lsp::json::Value item = json::MakeObject();
    item.object()["uri"] = std::string(uri);
    item.object()["languageId"] = std::string("angelscript");
    item.object()["version"] = version;
    item.object()["text"] = std::move(text);
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = std::move(item);
    return params;
}

void McpToolbox::SendChange(std::string_view uri, lsp::json::Value change)
{
    OpenState& state = m_open.try_emplace(std::string(uri)).first->second;
    lsp::json::Value id = json::MakeObject();
    id.object()["uri"] = std::string(uri);
    id.object()["version"] = ++state.version;
    lsp::json::Array changes;
    changes.push_back(std::move(change));
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = std::move(id);
    params.object()["contentChanges"] = std::move(changes);
    m_session.Notify("textDocument/didChange", std::move(params));
}

lsp::json::Value McpToolbox::OpenDocument(const lsp::json::Value& args)
{
    const std::string path = json::GetString(args, "path", true);
    const std::string uri = ResolveUri(path);
    std::string text = json::GetString(args, "text", false);
    if (!args.object().contains("text"))
    {
        std::ifstream file(lsp::Uri::parse(uri).fsPath(), std::ios::binary);
        if (!file.is_open())
        {
            throw std::runtime_error("cannot read '" + path + "'");
        }
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    OpenState& state = m_open[uri];
    if (state.version > 0)
    {
        lsp::json::Value change = json::MakeObject();
        change.object()["text"] = std::move(text);
        SendChange(uri, std::move(change));
    }
    else
    {
        state.version = 1;
        m_session.Notify("textDocument/didOpen", MakeOpenParams(uri, state.version, std::move(text)));
    }

    lsp::json::Value result = json::MakeObject();
    result.object()["uri"] = std::string(uri);
    result.object()["version"] = state.version;
    return result;
}

lsp::json::Value McpToolbox::ApplyEdit(const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    if (!m_open.contains(uri))
    {
        throw std::runtime_error("document is not open; call open_document first");
    }
    lsp::json::Value range = json::MakeObject();
    range.object()["start"] =
        json::MakePosition(json::GetNonNegativeInt(args, "startLine"), json::GetNonNegativeInt(args, "startCharacter"));
    range.object()["end"] =
        json::MakePosition(json::GetNonNegativeInt(args, "endLine"), json::GetNonNegativeInt(args, "endCharacter"));
    lsp::json::Value change = json::MakeObject();
    change.object()["range"] = std::move(range);
    change.object()["text"] = json::GetString(args, "text", true);
    SendChange(uri, std::move(change));

    lsp::json::Value result = json::MakeObject();
    result.object()["version"] = m_open[uri].version;
    return result;
}

lsp::json::Value McpToolbox::ReplaceDocument(const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    if (!m_open.contains(uri))
    {
        throw std::runtime_error("document is not open; call open_document first");
    }
    lsp::json::Value change = json::MakeObject();
    change.object()["text"] = json::GetString(args, "text", true);
    SendChange(uri, std::move(change));

    lsp::json::Value result = json::MakeObject();
    result.object()["version"] = m_open[uri].version;
    return result;
}

lsp::json::Value McpToolbox::Diagnostics(const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = json::MakeDocumentId(uri);
    lsp::json::Value report;
    for (int attempt = 0;; ++attempt)
    {
        try
        {
            report = m_session.Request("textDocument/diagnostic", params);
            break;
        }
        catch (const std::runtime_error&)
        {
            // The server answers ServerCancelled while it is still computing; ask again a few times.
            if (attempt >= k_diagnosticRetries)
            {
                throw;
            }
        }
    }
    if (report.isObject() && report.object().contains("items"))
    {
        return report.object().get("items");
    }
    return lsp::json::Value(lsp::json::Array());
}

lsp::json::Value McpToolbox::PositionRequest(std::string_view method, const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = json::MakeDocumentId(uri);
    params.object()["position"] =
        json::MakePosition(json::GetNonNegativeInt(args, "line"), json::GetNonNegativeInt(args, "character"));
    return m_session.Request(method, std::move(params));
}

lsp::json::Value McpToolbox::Completion(const lsp::json::Value& args)
{
    const lsp::json::Value result = PositionRequest("textDocument/completion", args);
    const lsp::json::Array* items = nullptr;
    if (result.isArray())
    {
        items = &result.array();
    }
    else if (result.isObject() && result.object().contains("items") && result.object().get("items").isArray())
    {
        items = &result.object().get("items").array();
    }

    int limit = k_defaultCompletionLimit;
    if (args.isObject() && args.object().contains("limit"))
    {
        limit = std::max(1, json::GetNonNegativeInt(args, "limit"));
    }

    lsp::json::Array trimmed;
    if (items != nullptr)
    {
        for (const lsp::json::Value& item : *items)
        {
            if (static_cast<int>(trimmed.size()) >= limit || !item.isObject())
            {
                break;
            }
            lsp::json::Value entry = json::MakeObject();
            for (const char* key : {"label", "kind", "detail"})
            {
                if (const lsp::json::Value* value = item.object().find(key))
                {
                    entry.object()[key] = *value;
                }
            }
            trimmed.push_back(std::move(entry));
        }
    }
    return lsp::json::Value(std::move(trimmed));
}

lsp::json::Value McpToolbox::DocumentSymbols(const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = json::MakeDocumentId(uri);
    return m_session.Request("textDocument/documentSymbol", std::move(params));
}

} // namespace angel_lsp::mcp
