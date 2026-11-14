#pragma once

#include "i18n/i18n.h"
#include "lsp/mcp/LspClientSession.h"

#include <lsp/json/json.h>
#include <map>
#include <string>
#include <string_view>

namespace angel_lsp::mcp
{

/**
 * @brief Compact description of one tool.
 */
struct ToolSpec
{
    /// Tool name as exposed over MCP.
    std::string_view name;
    /// i18n key of the description shown to the model.
    std::string_view descriptionKey;
    /// Arguments as "name:type" entries separated by commas; a trailing '!' marks a required one.
    std::string_view arguments;
};

/**
 * @brief The MCP tool set: each tool maps a few plain arguments onto LSP traffic, so a model can use the
 *        language server the way a person uses an editor.
 *
 * Paths are resolved against a root directory and may not escape it. Positions are zero-based and use the
 * LSP character unit (UTF-16 code units), exactly as the editor would send them.
 */
class McpToolbox
{
  public:
    /**
     * @param[in] session Client session every tool talks through; must outlive the toolbox.
     * @param[in] root Directory that relative paths resolve against and that no path may leave; empty allows any.
     * @param[in] locale Locale tag selecting the language of tool descriptions.
     */
    McpToolbox(LspClientSession& session, std::string_view root, std::string_view locale);

    /** @brief Builds the tools/list result: an array of {name, description, inputSchema}. */
    [[nodiscard]] lsp::json::Value ListTools() const;

    /**
     * @brief Runs one tool.
     * @param[in] name Tool name as listed by ListTools.
     * @param[in] args Arguments object; null is treated as empty.
     * @return The tool's JSON result.
     * @throws std::runtime_error for an unknown tool, a missing or invalid argument, or a path outside the root.
     */
    lsp::json::Value CallTool(std::string_view name, const lsp::json::Value& args);

  private:
    struct OpenState
    {
        int version = 0;
    };

    [[nodiscard]] std::string ResolveUri(std::string_view path) const;
    lsp::json::Value OpenDocument(const lsp::json::Value& args);
    lsp::json::Value ApplyEdit(const lsp::json::Value& args);
    lsp::json::Value ReplaceDocument(const lsp::json::Value& args);
    lsp::json::Value Diagnostics(const lsp::json::Value& args);
    lsp::json::Value PositionRequest(std::string_view method, const lsp::json::Value& args);
    lsp::json::Value Completion(const lsp::json::Value& args);
    lsp::json::Value DocumentSymbols(const lsp::json::Value& args);
    void SendChange(std::string_view uri, lsp::json::Value change);

    /** @throws std::runtime_error when the document was never opened through open_document. */
    void RequireOpen(std::string_view uri) const;

    LspClientSession& m_session;
    std::string m_root;
    i18n::I18n m_i18n;
    std::map<std::string, OpenState, std::less<>> m_open;
};

} // namespace angel_lsp::mcp
