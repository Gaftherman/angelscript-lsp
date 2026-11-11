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

lsp::SemanticTokens Server::ComputeAndCacheSemanticTokens(const std::string& uriStr, const std::string& text)
{
    const auto docHandle = GetDocumentStore().GetDocument(uriStr);
    const auto syntax = docHandle && docHandle->tree
                            ? ResolveProtocolSyntax(uriStr, docHandle->ParsedSource(), docHandle.get()).syntax
                            : nullptr;
    const analysis::SemanticTokenFacts facts =
        syntax ? analysis::CollectSemanticTokenFacts(syntax->index, text) : analysis::SemanticTokenFacts{};
    const int currentVersion = docHandle ? docHandle->version : GetDocumentStore().GetVersion(uriStr);

    features::SemanticTokensRequest request{uriStr, text, facts, m_symbolTable};
    request.scopeRoot = m_scopeIndex.GetRoot(uriStr);
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
        m_semanticTokensCache[uriStr] = SemanticTokensSnapshot{resultId, tokens.data, hasError, currentVersion};
    }

    return tokens;
}

std::string Server::CanonicalPathFromUri(const std::string& uriStr)
{
    const std::string path = angel_lsp::utils::UriToPath(uriStr);
    if (path.empty())
    {
        return "";
    }
    return angel_lsp::utils::IncludeResolver::NormalizePath(path);
}

std::string Server::DocumentKey(const std::string& uriStr)
{
    const std::string path = CanonicalPathFromUri(uriStr);
    return path.empty() ? uriStr : UriFromPath(path);
}

std::string Server::UriFromPath(const std::string& path)
{
    return angel_lsp::utils::PathToUri(angel_lsp::utils::IncludeResolver::NormalizePath(path));
}

std::shared_ptr<const std::string> Server::FindDocumentText(const std::string& uri) const
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

std::shared_ptr<const document::Document> Server::ResolveVirtualDocument(const std::string& key,
                                                                         const std::string& uriStr)
{
    const std::string& targetUri = key.starts_with("angelscript-virtual:") ? key : uriStr;
    std::string text = GenerateVirtualMixinDocument(targetUri);
    if (text.empty())
    {
        return nullptr;
    }
    parser::TreePtr tree = m_parser ? m_parser->ParseTree(text) : parser::MakeTreePtr(nullptr);
    GetDocumentStore().OpenDocument(DocumentStore::OpenDocumentRequest{key, text, 0, std::move(tree), key});
    return GetDocumentStore().GetDocument(key);
}

std::optional<OpenDocument> Server::LookupPreindexedFallback(const std::string& key)
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
            return OpenDocument{key, &preindexed->text, preindexed, nullptr};
        }
        return std::nullopt;
    }
    if (m_workspaceStore)
    {
        if (auto wsDoc = m_workspaceStore->GetPreindexedDocument(key))
        {
            return OpenDocument{key, &wsDoc->text, wsDoc, nullptr};
        }
    }
    return std::nullopt;
}

std::optional<OpenDocument> Server::LookupOpenDocument(const std::string& uriStr)
{
    const std::string key = DocumentKey(uriStr);

    auto doc = GetDocumentStore().GetDocument(key);
    if (!doc)
    {
        if (key.starts_with("angelscript-virtual:") || uriStr.starts_with("angelscript-virtual:"))
        {
            doc = ResolveVirtualDocument(key, uriStr);
            if (!doc)
            {
                return std::nullopt;
            }
        }
        else
        {
            return LookupPreindexedFallback(key);
        }
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
