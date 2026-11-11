#include "features/completion/CompletionHandler.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

lsp::requests::TextDocument_Completion::Result
Server::HandleRequestsTextDocument_Completion(lsp::requests::TextDocument_Completion::Params&& req)
{
    if (!m_config.features.enableCompletion)
    {
        return lsp::Array<lsp::CompletionItem>{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Array<lsp::CompletionItem>{};
    }

    const auto syntax =
        (doc->docHandle && doc->docHandle->tree)
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;
    const lsp::Position position = codec::Decode(*doc->text, m_sessionCoordinator->GetPositionEncoding(), req.position);
    const std::string documentPath = CanonicalPathFromUri(doc->uri);

    analysis::completion::CompletionQuery cr{doc->uri,
                                             *doc->text,
                                             syntax ? &syntax->index : nullptr,
                                             m_symbolTable,
                                             m_scopeIndex,
                                             {position.line, position.character},
                                             &m_config,
                                             m_sessionCoordinator->GetClientSupportsSnippets()};

    cr.documentPath = documentPath;
    cr.implicitExtension = ImplicitIncludeExtension();
    cr.listIncludeCandidates = [this]() { return IncludableFiles(); };
    cr.findModuleSymbols = [this](std::string_view prefix)
    {
        std::vector<std::pair<std::string, std::string>> results;
        for (const auto& sym : GetModuleIndex().FindSymbolsByPrefix(prefix))
        {
            results.emplace_back(sym.name, sym.containerName);
        }
        return results;
    };

    return features::GetCompletion(cr);
}

lsp::requests::CompletionItem_Resolve::Result
Server::HandleRequestsCompletionItem_Resolve(lsp::requests::CompletionItem_Resolve::Params&& req)
{
    if (!m_config.features.enableCompletion)
    {
        return req;
    }

    auto heldDocs = std::make_shared<std::vector<std::shared_ptr<const std::string>>>();
    features::CompletionResolveRequest rr{req, m_symbolTable,
                                          [this, heldDocs](const std::string& uri) -> const std::string*
                                          {
                                              auto sp = FindDocumentText(uri);
                                              if (!sp)
                                              {
                                                  return nullptr;
                                              }
                                              heldDocs->push_back(sp);
                                              return sp.get();
                                          }};
    return features::ResolveCompletionItem(rr);
}

lsp::requests::TextDocument_SignatureHelp::Result
Server::HandleRequestsTextDocument_SignatureHelp(lsp::requests::TextDocument_SignatureHelp::Params&& req)
{
    if (!m_config.features.enableSignatureHelp)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    const auto syntax =
        (doc->docHandle && doc->docHandle->tree)
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;

    features::SignatureHelpRequest sr{doc->uri,
                                      *doc->text,
                                      m_symbolTable,
                                      m_scopeIndex,
                                      codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                      syntax ? &syntax->index : nullptr};
    auto sig = features::GetSignatureHelp(sr);
    if (sig.has_value())
    {
        return sig.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_DocumentSymbol::Result
Server::HandleRequestsTextDocument_DocumentSymbol(lsp::requests::TextDocument_DocumentSymbol::Params&& req)
{
    if (!m_config.features.enableDocumentSymbols)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    utils::HighResTimer timer;
    features::DocumentSymbolRequest dr{doc->uri, *doc->text, m_symbolTable};
    auto symbols = features::GetDocumentSymbols(dr);
    const double elapsedMs = timer.ElapsedMs();
    if (symbols.has_value())
    {
        LogInfo(fmt::format("[Document Symbols] Finished in {:.2f} ms for {} ({} symbols)", elapsedMs, doc->uri,
                            symbols->size()));
        EncodeIn(*doc->text, symbols.value());
        return symbols.value();
    }
    LogInfo(fmt::format("[Document Symbols] Finished in {:.2f} ms for {} (0 symbols)", elapsedMs, doc->uri));
    return lsp::Null{};
}

lsp::requests::TextDocument_DocumentLink::Result
Server::HandleRequestsTextDocument_DocumentLink(lsp::requests::TextDocument_DocumentLink::Params&& req)
{
    if (!m_config.features.enableDocumentLink)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    const auto searchDirectories = SearchDirectories();

    const auto syntax =
        (doc->docHandle && doc->docHandle->tree)
            ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
            : nullptr;

    const auto allowedRoots = IncludeAllowedRoots();
    features::DocumentLinkRequest dlr{doc->uri,
                                      *doc->text,
                                      {.searchDirectories = *searchDirectories,
                                       .allowedRoots = allowedRoots,
                                       .implicitExtension = ImplicitIncludeExtension()},
                                      m_i18n.get()};
    dlr.nodeIndex = syntax ? &syntax->index : nullptr;
    auto links = features::GetDocumentLinks(dlr);
    if (links.has_value())
    {
        EncodeIn(*doc->text, links.value());
        return links.value();
    }
    return lsp::Null{};
}

lsp::requests::DocumentLink_Resolve::Result
Server::HandleRequestsDocumentLink_Resolve(lsp::requests::DocumentLink_Resolve::Params&& params)
{
    if (!m_config.features.enableDocumentLink)
    {
        return std::move(params);
    }

    // Document links are resolved eagerly during textDocument/documentLink, so the link
    // arrives already resolved and there is nothing left to compute. This handler exists
    // so a client that insists on the resolve round-trip gets a valid answer rather than
    // MethodNotFound.
    return std::move(params);
}

lsp::requests::InlayHint_Resolve::Result
Server::HandleRequestsInlayHint_Resolve(lsp::requests::InlayHint_Resolve::Params&& params)
{
    if (!m_config.features.enableInlayHints)
    {
        return std::move(params);
    }

    // Inlay hints are produced complete in textDocument/inlayHint, with all labels,
    // tooltips, and locations already computed. This handler returns the hint unchanged so
    // clients performing a resolve round-trip get a valid response instead of MethodNotFound.
    return std::move(params);
}

} // namespace angel_lsp
