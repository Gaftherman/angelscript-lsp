#include "lsp/mcp/McpJson.h"
#include "lsp/mcp/McpTools.h"

#include <string>
#include <utility>

namespace angel_lsp::mcp
{

/// Symbols returned when the caller gives no limit.
static constexpr int k_defaultSymbolLimit = 50;

/// Files returned by module_graph and matches returned by predefined_lookup when the caller gives no limit.
static constexpr int k_defaultProjectLimit = 500;

/** @brief Keeps the first `limit` entries of an array result and reports whether any were dropped. */
static lsp::json::Value TrimItems(const lsp::json::Value& result, int limit)
{
    lsp::json::Array kept;
    bool truncated = false;
    if (result.isArray())
    {
        for (const lsp::json::Value& item : result.array())
        {
            if (static_cast<int>(kept.size()) >= limit)
            {
                truncated = true;
                break;
            }
            kept.push_back(item);
        }
    }
    lsp::json::Value reply = json::MakeObject();
    reply.object()["truncated"] = truncated;
    reply.object()["items"] = lsp::json::Value(std::move(kept));
    return reply;
}

lsp::json::Value McpToolbox::References(const lsp::json::Value& args)
{
    const std::string uri = ResolveUri(json::GetString(args, "path", true));
    RequireOpen(uri);
    lsp::json::Value context = json::MakeObject();
    context.object()["includeDeclaration"] = json::GetBool(args, "includeDeclaration", true);
    lsp::json::Value params = json::MakeObject();
    params.object()["textDocument"] = json::MakeDocumentId(uri);
    params.object()["position"] =
        json::MakePosition(json::GetNonNegativeInt(args, "line"), json::GetNonNegativeInt(args, "character"));
    params.object()["context"] = std::move(context);
    return m_session.Request("textDocument/references", std::move(params));
}

lsp::json::Value McpToolbox::WorkspaceSymbols(const lsp::json::Value& args)
{
    lsp::json::Value params = json::MakeObject();
    params.object()["query"] = json::GetString(args, "query", true);
    return TrimItems(m_session.Request("workspace/symbol", std::move(params)),
                     json::GetLimit(args, "limit", k_defaultSymbolLimit));
}

lsp::json::Value McpToolbox::ModuleGraph(const lsp::json::Value& args)
{
    lsp::json::Value params = json::MakeObject();
    const std::string path = json::GetString(args, "path", false);
    if (!path.empty())
    {
        params.object()["uri"] = ResolveUri(path);
    }
    params.object()["limit"] = json::GetLimit(args, "limit", k_defaultProjectLimit);
    return m_session.Request("angelscript/moduleGraph", std::move(params));
}

lsp::json::Value McpToolbox::PredefinedLookup(const lsp::json::Value& args)
{
    lsp::json::Value params = json::MakeObject();
    params.object()["name"] = json::GetString(args, "name", true);
    params.object()["limit"] = json::GetLimit(args, "limit", 20);
    return m_session.Request("angelscript/predefinedLookup", std::move(params));
}

} // namespace angel_lsp::mcp
