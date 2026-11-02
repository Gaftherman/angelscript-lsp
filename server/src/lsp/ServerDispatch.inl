#pragma once

namespace angel_lsp
{

inline WorkspaceStateStore& Server::GetWorkspaceStore() noexcept { return *m_workspaceStore; }
inline const WorkspaceStateStore& Server::GetWorkspaceStore() const noexcept { return *m_workspaceStore; }
inline DiagnosticsPipeline& Server::GetDiagnosticsPipeline() noexcept { return *m_diagnosticsPipeline; }
inline const DiagnosticsPipeline& Server::GetDiagnosticsPipeline() const noexcept { return *m_diagnosticsPipeline; }
inline LspSessionCoordinator& Server::GetSessionCoordinator() noexcept { return *m_sessionCoordinator; }
inline const LspSessionCoordinator& Server::GetSessionCoordinator() const noexcept { return *m_sessionCoordinator; }
inline const analysis::SymbolTable& Server::GetSymbolTable() const { return m_symbolTable; }
inline analysis::SymbolTable& Server::GetMutableSymbolTable() noexcept { return m_symbolTable; }
inline PredefinedStubManager& Server::GetPredefinedManager() noexcept { return m_predefinedManager; }
inline DocumentStore& Server::GetDocumentStore() noexcept { return m_workspaceStore->GetDocumentStore(); }
inline const DocumentStore& Server::GetDocumentStore() const noexcept { return m_workspaceStore->GetDocumentStore(); }
inline utils::WorkspaceIncludeGraph& Server::GetIncludeGraph() noexcept { return m_workspaceStore->GetIncludeGraph(); }
inline const utils::WorkspaceIncludeGraph& Server::GetIncludeGraph() const noexcept { return m_workspaceStore->GetIncludeGraph(); }
inline ModuleIndex& Server::GetModuleIndex() noexcept { return m_workspaceStore->GetModuleIndex(); }
inline const ModuleIndex& Server::GetModuleIndex() const noexcept { return m_workspaceStore->GetModuleIndex(); }
inline utils::PositionEncoding Server::GetPositionEncoding() const noexcept { return m_sessionCoordinator->GetPositionEncoding(); }
inline std::vector<std::string> Server::IncludableFiles() const { return m_workspaceStore->GetIncludeGraph().AllFiles(); }
inline void Server::SetOnBeforeCommitHook(OnBeforeCommitHook hook) { m_onBeforeCommitHook = std::move(hook); }
inline int Server::GetDocumentVersion(const std::string& uriStr) const { return GetDocumentStore().GetVersion(uriStr); }
inline void Server::ClearMixinCaches() { m_serverMixinCache.clear(); m_clearWorkerMixinCache.store(true, std::memory_order_release); }
inline void Server::LogInfo(std::string_view message) const { if (m_logger) m_logger->LogInfo(message); }
inline void Server::LogWarning(std::string_view message) const { if (m_logger) m_logger->LogWarning(message); }
inline void Server::LogError(std::string_view message) const { if (m_logger) m_logger->LogError(message); }
inline void Server::LogDebug(std::string_view message) const { if (m_logger) m_logger->LogDebug(message); }
inline void Server::LogTrace(std::string_view message) const { if (m_logger) m_logger->LogTrace(message); }
inline const angel_lsp::config::ServerConfig& Server::GetConfig() const noexcept { return m_config; }
inline angel_lsp::config::ServerConfig& Server::GetMutableConfig() noexcept { return m_config; }
inline const analysis::CallGraphIndex& Server::GetCallGraph() const noexcept { return m_callGraph; }

template <typename ResultT, typename FeatureFn>
ResultT Server::ExecuteOnOpenDocument(const std::string& uriStr, bool featureEnabled, FeatureFn&& fn)
{
    if (!featureEnabled)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return lsp::Null{};
    }
    return fn(*doc);
}

template <typename ResultT, typename FeatureFn>
ResultT Server::ExecuteAtDocumentPosition(const std::string& uriStr, const lsp::Position& rawPos, bool featureEnabled,
                                         FeatureFn&& fn)
{
    if (!featureEnabled)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return lsp::Null{};
    }
    const lsp::Position decodedPos = codec::Decode(*doc->text, GetPositionEncoding(), rawPos);
    return fn(*doc, decodedPos);
}

template <typename ItemT>
void Server::EncodeItemRanges(ItemT& item) const
{
    if (GetPositionEncoding() == utils::PositionEncoding::Utf8)
    {
        return;
    }
    if (auto text = FindDocumentText(item.uri.toString()))
    {
        EncodeIn(*text, item.range);
        EncodeIn(*text, item.selectionRange);
    }
}

} // namespace angel_lsp
