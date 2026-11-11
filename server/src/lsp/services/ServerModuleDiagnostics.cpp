#include "features/document_link/DocumentLinkHandler.h"
#include "lsp/Server.h"
#include "lsp/services/ServerModuleUtils.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <algorithm>
#include <fstream>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <vector>

namespace angel_lsp
{
void Server::NotifyModuleWarning(const std::string& text)
{
    LogError(text);

    lsp::notifications::Window_ShowMessage::Params params;
    params.type = lsp::MessageType::Warning;
    params.message = text;

    std::lock_guard<std::mutex> lock(m_messageHandlerMutex);
    m_messageHandler->sendNotification<lsp::notifications::Window_ShowMessage>(std::move(params));
}

bool Server::IsOpenElsewhere(const std::string& uriStr) const
{
    return GetDocumentStore().IsOpen(uriStr);
}

void Server::ScheduleOpenDocumentsForReanalysis()
{
    const auto open = GetDocumentStore().GetSnapshot();
    for (const auto& [uriStr, text] : open)
    {
        if (angel_lsp::utils::IsPredefinedFile(uriStr, m_config.info.predefinedFileExtension))
        {
            continue;
        }
        ScheduleAnalysis(uriStr, text, true);
    }

    if (m_logger)
    {
        LogInfo(fmt::format("Re-analysing {} open document(s) now the workspace is indexed", open.size()));
    }
}

void Server::AnalyzeConfiguredModules()
{
    if (m_modules.empty())
    {
        return;
    }

    for (const auto& view : m_modules)
    {
        if (view.isImplicitEntryPoint)
        {
            continue;
        }

        for (const auto& path : view.memberPaths)
        {
            const std::string uriStr = UriFromPath(path);

            if (IsOpenElsewhere(uriStr))
            {
                continue;
            }

            const ModuleClaim claim = ClaimFor(path);
            if (claim.owner == nullptr || claim.owner->name != view.name)
            {
                continue;
            }

            std::string content;
            {
                std::lock_guard<std::mutex> lock(m_closureMutex);
                if (const auto cached = m_closureDocuments.find(uriStr);
                    cached != m_closureDocuments.end() && cached->second.text)
                {
                    content = *cached->second.text;
                }
            }
            if (content.empty())
            {
                std::ifstream file(path, std::ios::binary);
                if (!file.is_open())
                {
                    continue;
                }
                content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }

            {
                std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
                m_publishedForModules.insert(uriStr);
            }
            ScheduleAnalysis(uriStr, content);
        }
    }
}

void Server::WithdrawStaleModuleDiagnostics()
{
    ankerl::unordered_dense::set<std::string> stale;

    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        for (const auto& uriStr : m_publishedForModules)
        {
            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty() || ClaimFor(path).owner == nullptr)
            {
                stale.insert(uriStr);
            }
        }
    }

    if (!m_config.modules.empty())
    {
        for (const auto& uriStr : GetDocumentStore().GetOpenUris())
        {
            const std::string path = CanonicalPathFromUri(uriStr);
            if (path.empty() || ClaimFor(path).owner == nullptr)
            {
                stale.insert(uriStr);
            }
        }
    }

    for (const auto& uriStr : stale)
    {
        PublishDiagnostics(uriStr, std::string(), {});
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        m_publishedForModules.erase(uriStr);
    }

    if (!stale.empty())
    {
        LogInfo(fmt::format("Withdrew module diagnostics for {} file(s)", stale.size()));
    }
}

std::string Server::ReadClosedDocumentContent(const std::string& uriStr)
{
    {
        std::lock_guard<std::mutex> lock(m_closureMutex);
        if (const auto cached = m_closureDocuments.find(uriStr);
            cached != m_closureDocuments.end() && cached->second.text)
        {
            return *cached->second.text;
        }
    }
    const std::string path = CanonicalPathFromUri(uriStr);
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void Server::ReanalyzeClosedModuleDependents(const std::string& savedUriStr, const ModuleClaim& savedClaim,
                                             const ankerl::unordered_dense::set<std::string>& closureSet)
{
    std::vector<std::string> closedToReanalyze;
    {
        std::lock_guard<std::mutex> lock(m_publishedForModulesMutex);
        for (const auto& pubUri : m_publishedForModules)
        {
            if (pubUri == savedUriStr || GetDocumentStore().IsOpen(pubUri))
            {
                continue;
            }
            const std::string pubPath = CanonicalPathFromUri(pubUri);
            if (pubPath.empty())
            {
                continue;
            }
            const bool sameModule = (savedClaim.owner != nullptr && ClaimFor(pubPath).owner == savedClaim.owner);
            if (sameModule || closureSet.contains(pubPath))
            {
                closedToReanalyze.push_back(pubUri);
            }
        }
    }

    for (const auto& pubUri : closedToReanalyze)
    {
        std::string content = ReadClosedDocumentContent(pubUri);
        if (!content.empty())
        {
            ScheduleAnalysis(pubUri, content);
        }
    }
}

void Server::AppendIncludeDiagnostics(const std::string& uriStr, const std::string& text,
                                      const std::vector<utils::ExcludedLineRange>& excluded,
                                      std::vector<angel_lsp::analysis::Diagnostic>& diagnostics)
{
    if (!m_config.features.enableDocumentLink)
    {
        return;
    }

    const auto searchDirectories = SearchDirectories();
    const auto doc = LookupOpenDocument(uriStr);
    const auto syntax =
        doc && doc->tree ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
                         : nullptr;

    const auto allowedRoots = IncludeAllowedRoots();
    features::DocumentLinkRequest request{uriStr,
                                          text,
                                          {.searchDirectories = *searchDirectories,
                                           .allowedRoots = allowedRoots,
                                           .implicitExtension = ImplicitIncludeExtension()},
                                          m_i18n.get()};
    request.excludedLineRanges = excluded;
    request.nodeIndex = syntax ? &syntax->index : nullptr;

    auto includeDiagnostics = features::GetUnresolvedIncludeDiagnostics(request);
    diagnostics.insert(diagnostics.end(), includeDiagnostics.begin(), includeDiagnostics.end());
}

std::vector<angel_lsp::utils::ExcludedLineRange> Server::ExcludedLineRanges(const std::string& text) const
{
    return angel_lsp::utils::FindExcludedLineRanges(text, *DefinedWords(), m_config.preprocessor);
}
} // namespace angel_lsp
