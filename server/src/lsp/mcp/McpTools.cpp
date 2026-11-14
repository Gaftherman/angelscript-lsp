#include "lsp/mcp/McpTools.h"

#include "lsp/mcp/McpJson.h"

#include <filesystem>
#include <lsp/uri.h>
#include <system_error>

namespace angel_lsp::mcp
{

static constexpr ToolSpec k_tools[] = {
    {"open_document", "mcp-tool-open-document", "path:string!,text:string"},
    {"apply_edit", "mcp-tool-apply-edit",
     "path:string!,startLine:integer!,startCharacter:integer!,endLine:integer!,endCharacter:integer!,text:string!"},
    {"replace_document", "mcp-tool-replace-document", "path:string!,text:string!"},
    {"diagnostics", "mcp-tool-diagnostics", "path:string!"},
    {"hover", "mcp-tool-hover", "path:string!,line:integer!,character:integer!"},
    {"completion", "mcp-tool-completion", "path:string!,line:integer!,character:integer!,limit:integer"},
    {"signature_help", "mcp-tool-signature-help", "path:string!,line:integer!,character:integer!"},
    {"definition", "mcp-tool-definition", "path:string!,line:integer!,character:integer!"},
    {"document_symbols", "mcp-tool-document-symbols", "path:string!"},
};

/** @brief Turns a "name:type!,name:type" list into a JSON Schema object. */
static lsp::json::Value BuildSchema(std::string_view arguments)
{
    lsp::json::Value schema = json::MakeObject();
    schema.object()["type"] = std::string("object");
    lsp::json::Value properties = json::MakeObject();
    lsp::json::Array required;

    while (!arguments.empty())
    {
        const std::size_t comma = arguments.find(',');
        std::string_view entry = arguments.substr(0, comma);
        arguments = (comma == std::string_view::npos) ? std::string_view{} : arguments.substr(comma + 1);

        const bool isRequired = !entry.empty() && entry.back() == '!';
        if (isRequired)
        {
            entry.remove_suffix(1);
        }
        const std::size_t colon = entry.find(':');
        const std::string name(entry.substr(0, colon));
        lsp::json::Value property = json::MakeObject();
        property.object()["type"] = std::string(entry.substr(colon + 1));
        properties.object()[name] = std::move(property);
        if (isRequired)
        {
            required.push_back(lsp::json::Value(std::string(name)));
        }
    }
    schema.object()["properties"] = std::move(properties);
    schema.object()["required"] = std::move(required);
    return schema;
}

McpToolbox::McpToolbox(LspClientSession& session, std::string_view root, std::string_view locale)
    : m_session(session), m_root(root), m_i18n(locale)
{
}

lsp::json::Value McpToolbox::ListTools() const
{
    lsp::json::Array tools;
    for (const ToolSpec& spec : k_tools)
    {
        lsp::json::Value tool = json::MakeObject();
        tool.object()["name"] = std::string(spec.name);
        tool.object()["description"] = i18n::FormatMessage(&m_i18n, spec.descriptionKey, spec.name);
        tool.object()["inputSchema"] = BuildSchema(spec.arguments);
        tools.push_back(std::move(tool));
    }
    return lsp::json::Value(std::move(tools));
}

std::string McpToolbox::ResolveUri(std::string_view path) const
{
    if (path.empty())
    {
        throw std::runtime_error("path is empty");
    }
    std::filesystem::path candidate(path);
    const bool hasRoot = !m_root.empty();
    if (candidate.is_relative() && hasRoot)
    {
        candidate = std::filesystem::path(m_root) / candidate;
    }

    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
    {
        throw std::runtime_error("cannot resolve path");
    }
    if (hasRoot)
    {
        const std::filesystem::path root = std::filesystem::weakly_canonical(std::filesystem::path(m_root), ec);
        const std::filesystem::path relative = resolved.lexically_relative(root);
        if (ec || relative.empty() || *relative.begin() == "..")
        {
            throw std::runtime_error("path is outside the root");
        }
    }
    return lsp::Uri::fileUriFromPath(resolved.generic_string()).toString();
}

lsp::json::Value McpToolbox::CallTool(std::string_view name, const lsp::json::Value& args)
{
    if (name == "open_document")
    {
        return OpenDocument(args);
    }
    if (name == "apply_edit")
    {
        return ApplyEdit(args);
    }
    if (name == "replace_document")
    {
        return ReplaceDocument(args);
    }
    if (name == "diagnostics")
    {
        return Diagnostics(args);
    }
    if (name == "hover")
    {
        return PositionRequest("textDocument/hover", args);
    }
    if (name == "signature_help")
    {
        return PositionRequest("textDocument/signatureHelp", args);
    }
    if (name == "definition")
    {
        return PositionRequest("textDocument/definition", args);
    }
    if (name == "completion")
    {
        return Completion(args);
    }
    if (name == "document_symbols")
    {
        return DocumentSymbols(args);
    }
    throw std::runtime_error("unknown tool '" + std::string(name) + "'");
}

} // namespace angel_lsp::mcp
