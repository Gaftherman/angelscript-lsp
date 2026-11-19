#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "parser/SyntaxTree.h"
#include "utils/IncludeResolver.h"
#include "utils/Utils.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

lsp::SemanticTokens Server::ComputeAndCacheSemanticTokens(std::string_view uriStr, std::string_view text)
{
    const auto docHandle = GetDocumentStore().GetDocument(uriStr);
    const int currentVersion = docHandle ? docHandle->version : GetDocumentStore().GetVersion(uriStr);
    const uint64_t tableVersion = m_symbolTable.Version();
    const auto cachedScopeRoot = m_scopeIndex.GetRoot(uriStr);

    // The tokens are a pure function of the text, the symbol table and the scope tree, so a request that
    // sees the same three as the cached entry gets the cached answer without a second classification pass.
    if (currentVersion >= 0)
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        if (const auto cached = m_semanticTokensCache.find(uriStr);
            cached != m_semanticTokensCache.end() && cached->second.version == currentVersion &&
            cached->second.tableVersion == tableVersion && cached->second.scopeIdentity == cachedScopeRoot.get())
        {
            lsp::SemanticTokens hit;
            hit.resultId = cached->second.resultId;
            hit.data = cached->second.data;
            return hit;
        }
    }

    const auto syntax = docHandle && docHandle->tree
                            ? ResolveProtocolSyntax(uriStr, docHandle->ParsedSource(), docHandle.get()).syntax
                            : nullptr;
    const analysis::SemanticTokenFacts facts =
        syntax ? analysis::CollectSemanticTokenFacts(syntax->index, text) : analysis::SemanticTokenFacts{};

    features::SemanticTokensRequest request{uriStr, text, facts, m_symbolTable};
    request.scopeRoot = cachedScopeRoot;
    if (syntax && m_localScopeCollector && !request.scopeRoot)
    {
        request.scopeRoot = m_localScopeCollector->CollectScopesFromIndex(syntax->index, text);
    }
    request.excludedLineRanges = ExcludedLineRanges(text);
    lsp::SemanticTokens tokens = features::GetSemanticTokens(request);
    codec::EncodeSemanticTokens(text, m_sessionCoordinator->GetPositionEncoding(), tokens.data);

    const bool hasError = syntax && parser::TreeHasError(syntax->tree);

    // Cached after encoding, so a delta is computed against exactly the bytes the client holds.
    const std::string resultId = std::to_string(++m_semanticTokensRevision);
    tokens.resultId = resultId;
    {
        std::lock_guard<std::mutex> lock(m_semanticTokensMutex);
        m_semanticTokensCache.insert_or_assign(std::string(uriStr),
                                               SemanticTokensSnapshot{resultId, tokens.data, hasError, currentVersion,
                                                                      tableVersion, cachedScopeRoot.get()});
    }

    return tokens;
}

std::string Server::CanonicalPathFromUri(std::string_view uriStr)
{
    const std::string path = angel_lsp::utils::UriToPath(uriStr);
    if (path.empty())
    {
        return "";
    }
    return angel_lsp::utils::IncludeResolver::NormalizePath(path);
}

std::string Server::DocumentKey(std::string_view uriStr)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    return path.empty() ? std::string(uriStr) : UriFromPath(path);
}

std::string Server::UriFromPath(std::string_view path)
{
    return angel_lsp::utils::PathToUri(angel_lsp::utils::IncludeResolver::NormalizePath(path));
}

std::shared_ptr<const std::string> Server::FindDocumentText(std::string_view uri) const
{
    const std::string key = DocumentKey(uri);

    if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
    {
        if (auto predefined = m_predefinedManager.GetDocumentTextShared(key))
        {
            return predefined;
        }
    }

    if (auto open = GetDocumentStore().GetTextShared(key))
    {
        return open;
    }

    if (auto predefined = m_predefinedManager.GetDocumentTextShared(key))
    {
        return predefined;
    }

    {
        std::lock_guard lock(m_closureMutex);
        if (const auto it = m_closureDocuments.find(key); it != m_closureDocuments.end())
        {
            return it->second.text;
        }
    }

    return nullptr;
}

std::optional<OpenDocument> Server::LookupPreindexedFallback(std::string_view key)
{
    if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
    {
        auto preindexed = m_predefinedManager.GetPreindexedDocument(key);
        if (!preindexed && m_workspaceStore)
        {
            preindexed = m_workspaceStore->GetPreindexedDocument(key);
        }
        if (preindexed)
        {
            return OpenDocument{std::string(key), &preindexed->text, preindexed, nullptr};
        }
        return std::nullopt;
    }
    if (m_workspaceStore)
    {
        if (auto wsDoc = m_workspaceStore->GetPreindexedDocument(key))
        {
            return OpenDocument{std::string(key), &wsDoc->text, wsDoc, nullptr};
        }
    }
    return std::nullopt;
}

std::optional<OpenDocument> Server::LookupOpenDocument(std::string_view uriStr)
{
    const std::string key = DocumentKey(uriStr);

    auto doc = GetDocumentStore().GetDocument(key);
    if (!doc)
    {
        return LookupPreindexedFallback(key);
    }

    const std::string* textPtr = doc ? &doc->text : nullptr;
    std::shared_ptr<const std::string> predefinedHandle;

    if (angel_lsp::utils::IsPredefinedFile(key, m_config.info.predefinedFileExtension))
    {
        predefinedHandle = m_predefinedManager.GetDocumentTextShared(key);
        if (predefinedHandle)
        {
            textPtr = predefinedHandle.get();
        }
    }

    return OpenDocument{key, textPtr, doc, predefinedHandle};
}

} // namespace angel_lsp
