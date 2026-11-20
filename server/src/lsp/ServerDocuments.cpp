#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "parser/SyntaxTree.h"
#include "utils/IncludeResolver.h"
#include "utils/PreprocessorRegions.h"
#include "utils/Utils.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

SemanticTokensRevisionKey
Server::CaptureSemanticTokensRevisionKey(const document::Document& doc,
                                         const std::shared_ptr<const analysis::Scope>& scopeRoot,
                                         const std::shared_ptr<const TransparentStringSet>& words) const
{
    return {doc.generation,
            doc.version,
            m_symbolTable.Version(),
            m_sessionCoordinator->GetConfigRevision(),
            GetPositionEncoding(),
            scopeRoot,
            words};
}

std::optional<SemanticTokensSnapshot> Server::FindSemanticTokensSnapshot(std::string_view uri)
{
    std::lock_guard lock(m_semanticTokensMutex);
    const auto found = m_semanticTokensCache.find(uri);
    return found == m_semanticTokensCache.end() ? std::nullopt : std::optional{found->second};
}

SemanticTokensSnapshot Server::ResolveSemanticTokensSnapshot(const OpenDocument& doc)
{
    const bool cacheable = doc.docHandle && doc.docHandle->generation != 0 && doc.docHandle->version >= 0 &&
                           doc.text == &doc.docHandle->text;
    // Analysis can publish a new semantic revision while tokens are being classified.
    // Retry once for the same document so that a background publication need not force a full response.
    for (bool retried = false;; retried = true)
    {
        std::shared_ptr<const analysis::Scope> scopeRoot;
        std::shared_ptr<const TransparentStringSet> words;
        SemanticTokensRevisionKey key;
        std::optional<SemanticTokensSnapshot> cached;
        {
            std::lock_guard lifecycleLock(m_lifecycleMutex);
            scopeRoot = m_scopeIndex.GetRoot(doc.uri);
            words = DefinedWords();
            if (cacheable)
            {
                key = CaptureSemanticTokensRevisionKey(*doc.docHandle, scopeRoot, words);
                cached = FindSemanticTokensSnapshot(doc.uri);
            }
            else
            {
                key.encoding = GetPositionEncoding();
            }
        }
        if (cached && cached->key == key)
        {
            return std::move(*cached);
        }

        const auto syntax =
            doc.docHandle && doc.docHandle->tree
                ? ResolveProtocolSyntax(doc.uri, doc.docHandle->ParsedSource(), doc.docHandle.get()).syntax
                : nullptr;
        const std::string_view text = *doc.text;
        const analysis::SemanticTokenFacts facts =
            syntax ? analysis::CollectSemanticTokenFacts(syntax->index, text) : analysis::SemanticTokenFacts{};

        features::SemanticTokensRequest request{doc.uri, text, facts, m_symbolTable};
        request.scopeRoot = scopeRoot;
        if (syntax && m_localScopeCollector && !request.scopeRoot)
        {
            request.scopeRoot = m_localScopeCollector->CollectScopesFromIndex(syntax->index, text);
        }
        request.excludedLineRanges = utils::FindExcludedLineRanges(text, *words, m_config.preprocessor);
        auto tokens = features::GetSemanticTokens(request);
        codec::EncodeSemanticTokens(text, key.encoding, tokens.data);

        SemanticTokensSnapshot snapshot{{},
                                        std::make_shared<const std::vector<lsp::uint>>(std::move(tokens.data)),
                                        syntax && parser::TreeHasError(syntax->tree),
                                        key};
        if (!cacheable)
        {
            return snapshot;
        }

        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (GetDocumentStore().GetDocument(doc.uri) != doc.docHandle)
        {
            return snapshot;
        }
        if (CaptureSemanticTokensRevisionKey(*doc.docHandle, m_scopeIndex.GetRoot(doc.uri), DefinedWords()) == key)
        {
            std::lock_guard cacheLock(m_semanticTokensMutex);
            snapshot.resultId = std::to_string(++m_semanticTokensRevision);
            m_semanticTokensCache.insert_or_assign(doc.uri, snapshot);
            return snapshot;
        }
        if (retried)
        {
            return snapshot;
        }
    }
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
