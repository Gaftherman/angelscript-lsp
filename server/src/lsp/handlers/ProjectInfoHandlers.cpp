#include "analysis/documentation/DocComment.h"
#include "analysis/documentation/HoverMarkdown.h"
#include "lsp/Server.h"
#include "lsp/services/ProjectInfoJson.h"
#include "utils/Utils.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

/** @brief Matches returned by angelscript/predefinedLookup when the request gives no limit. */
static constexpr int k_defaultLookupLimit = 20;

/** @brief Longest wait for the initial workspace scan before project facts are reported as they stand. */
static constexpr std::chrono::seconds k_scanWait{30};

/** @brief Files returned by angelscript/moduleGraph when the request gives no limit. */
static constexpr int k_defaultGraphLimit = 500;

/** @brief Reads a string member of a request, or empty when absent. */
static std::string RequestString(const lsp::json::Value& params, std::string_view key)
{
    const lsp::json::Value* value = params.isObject() ? params.object().find(key) : nullptr;
    return (value != nullptr && value->isString()) ? value->string() : std::string();
}

/** @brief Reads a positive integer member of a request, or the fallback when absent. */
static int RequestLimit(const lsp::json::Value& params, std::string_view key, int fallback)
{
    const lsp::json::Value* value = params.isObject() ? params.object().find(key) : nullptr;
    return (value != nullptr && value->isInteger() && value->integer() > 0) ? value->integer() : fallback;
}

lsp::json::Value Server::HandleRequestsAngelscript_WorkspaceInfo([[maybe_unused]] lsp::json::Value&& params)
{
    WaitForWorkspaceScan(k_scanWait);
    WorkspaceInfoView view;
    const std::vector<std::string> roots = WorkspaceRootPaths();
    view.root = roots.empty() ? std::string() : roots.front();
    if (!m_config.moduleEntryPoint.empty())
    {
        view.moduleEntryPoint = ResolveConfiguredPath(m_config.moduleEntryPoint);
    }
    for (const auto& [path, uri] : m_predefinedManager.GetAllUriByPath())
    {
        view.activePredefined.push_back(path);
    }
    std::sort(view.activePredefined.begin(), view.activePredefined.end());
    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        view.rulesFile = m_effectiveRulesPath;
    }
    if (const auto directories = SearchDirectories())
    {
        view.includeRoots = *directories;
    }
    view.modules = m_config.modules;
    return BuildWorkspaceInfoJson(view);
}

lsp::json::Value Server::HandleRequestsAngelscript_EngineRules([[maybe_unused]] lsp::json::Value&& params)
{
    WaitForWorkspaceScan(k_scanWait);
    std::string rulesFile;
    {
        std::lock_guard<std::mutex> lock(m_runtimeConfigMutex);
        rulesFile = m_effectiveRulesPath;
    }
    return BuildEngineRulesJson(m_config.engineRules, m_config.engine.noHandleTypes, rulesFile);
}

lsp::json::Value Server::HandleRequestsAngelscript_ModuleGraph(lsp::json::Value&& params)
{
    WaitForWorkspaceScan(k_scanWait);
    ModuleGraphView view;
    const std::vector<std::string> roots = WorkspaceRootPaths();
    view.root = roots.empty() ? std::string() : roots.front();
    view.modules = m_config.modules;

    const std::string uri = RequestString(params, "uri");
    if (!uri.empty())
    {
        view.entry = utils::UriToPath(uri);
    }
    else if (!m_config.moduleEntryPoint.empty())
    {
        view.entry = ResolveConfiguredPath(m_config.moduleEntryPoint);
    }

    if (!view.entry.empty())
    {
        view.files = GetIncludeGraph().GetForwardClosure(view.entry);
        std::sort(view.files.begin(), view.files.end());
        const auto limit = static_cast<size_t>(RequestLimit(params, "limit", k_defaultGraphLimit));
        if (view.files.size() > limit)
        {
            view.files.resize(limit);
            view.truncated = true;
        }
    }
    return BuildModuleGraphJson(view);
}

lsp::json::Value Server::HandleRequestsAngelscript_PredefinedLookup(lsp::json::Value&& params)
{
    WaitForWorkspaceScan(k_scanWait);
    const std::string name = RequestString(params, "name");
    if (name.empty())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "name is required");
    }

    std::vector<analysis::Symbol> found = m_symbolTable.FindSymbols(name);
    if (found.empty())
    {
        found = m_symbolTable.FindTypeSymbolsByShortName(name);
    }
    std::erase_if(found, [](const analysis::Symbol& symbol) { return !symbol.isPredefined; });

    const std::vector<std::string> roots = WorkspaceRootPaths();
    const std::string root = roots.empty() ? std::string() : roots.front();
    const auto limit = static_cast<size_t>(RequestLimit(params, "limit", k_defaultLookupLimit));

    lsp::json::Array matches;
    for (const analysis::Symbol& symbol : found)
    {
        if (matches.size() >= limit)
        {
            break;
        }
        lsp::json::Value entry = lsp::json::Value(lsp::json::Object());
        entry.object()["kind"] = std::string(analysis::SymbolTypeToStringView(symbol.type));
        entry.object()["qualifiedName"] = std::string(symbol.qualifiedName);
        entry.object()["declaration"] = analysis::FormatDeclarationText(symbol, &m_symbolTable);
        entry.object()["file"] = RelativeToRoot(root, utils::UriToPath(symbol.fileUri));
        entry.object()["line"] = static_cast<int>(symbol.startLine);
        const std::optional<std::string> source = m_predefinedManager.GetDocumentText(symbol.fileUri);
        entry.object()["documentation"] =
            source.has_value() ? analysis::ExtractDocComment(*source, symbol.startLine) : std::string();
        matches.push_back(std::move(entry));
    }

    lsp::json::Value reply = lsp::json::Value(lsp::json::Object());
    reply.object()["name"] = std::string(name);
    reply.object()["matches"] = lsp::json::Value(std::move(matches));
    reply.object()["truncated"] = found.size() > limit;
    return reply;
}

} // namespace angel_lsp
