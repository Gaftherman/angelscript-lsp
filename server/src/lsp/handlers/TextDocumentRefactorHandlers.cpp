#include "features/code_action/CodeActionHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "features/rename/RenameHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp
{

std::optional<RenameContext> Server::SetupRenameContext(const std::string& uriStr, const lsp::Position& rawPos)
{
    if (!m_config.features.enableRename)
    {
        return std::nullopt;
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return std::nullopt;
    }

    const auto loadedUris = m_predefinedManager.GetLoadedUris();
    return RenameContext{*doc, std::unordered_set<std::string>(loadedUris.begin(), loadedUris.end()),
                         codec::Decode(*doc->text, GetPositionEncoding(), rawPos)};
}

lsp::requests::TextDocument_PrepareRename::Result
Server::HandleRequestsTextDocument_PrepareRename(lsp::requests::TextDocument_PrepareRename::Params&& req)
{
    const auto ctx = SetupRenameContext(req.textDocument.uri.toString(), req.position);
    if (!ctx)
    {
        return lsp::Null{};
    }

    features::PrepareRenameRequest pr{ctx->doc.uri,
                                      *ctx->doc.text,
                                      ctx->decodedPosition,
                                      m_symbolTable,
                                      m_scopeIndex,
                                      [&uris = ctx->predefinedUris](std::string_view uri) { return uris.contains(std::string(uri)); },
                                      m_logger.get()};
    auto prep = features::PrepareRename(pr);
    if (prep.has_value())
    {
        EncodeIn(*ctx->doc.text, prep.value());
        return prep.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_Rename::Result
Server::HandleRequestsTextDocument_Rename(lsp::requests::TextDocument_Rename::Params&& req)
{
    const auto ctx = SetupRenameContext(req.textDocument.uri.toString(), req.position);
    if (!ctx)
    {
        return lsp::Null{};
    }

    features::RenameRequest rr{ctx->doc.uri,
                               *ctx->doc.text,
                               ctx->decodedPosition,
                               req.newName,
                               m_symbolTable,
                               m_scopeIndex,
                               [&uris = ctx->predefinedUris](std::string_view uri)
                               { return uris.contains(std::string(uri)); },
                               m_logger.get()};
    auto edit = features::Rename(rr);
    if (edit.has_value())
    {
        EncodeAcrossDocuments(edit.value());
        return edit.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_CodeAction::Result
Server::HandleRequestsTextDocument_CodeAction(lsp::requests::TextDocument_CodeAction::Params&& req)
{
    if (!m_config.features.enableCodeAction)
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

    lsp::CodeActionContext context = req.context;
    for (auto& diag : context.diagnostics)
    {
        diag.range = codec::Decode(*doc->text, GetPositionEncoding(), diag.range);
    }

    const auto allowedRoots = IncludeAllowedRoots();
    features::CodeActionRequest car{
        doc->uri,      *doc->text,   codec::Decode(*doc->text, GetPositionEncoding(), req.range), context,
        m_symbolTable, m_scopeIndex, utils::IncludeSearchScope{.allowedRoots = allowedRoots},     &m_config,
        m_i18n.get()};
    car.nodeIndex = syntax ? &syntax->index : nullptr;
    auto actions = features::GetCodeActions(car);
    if (actions.has_value())
    {
        EncodeAcrossDocuments(*actions);
        lsp::Array<lsp::OneOf<lsp::Command, lsp::CodeAction>> resultList;
        resultList.reserve(actions->size());
        for (auto& action : *actions)
        {
            resultList.push_back(std::move(action));
        }
        return resultList;
    }
    return lsp::Null{};
}

lsp::requests::CodeAction_Resolve::Result
Server::HandleRequestsCodeAction_Resolve(lsp::requests::CodeAction_Resolve::Params&& req)
{
    if (!m_config.features.enableCodeAction)
    {
        return req;
    }

    features::CodeActionResolveRequest carr{req, m_symbolTable, m_scopeIndex};
    auto resolved = features::ResolveCodeAction(carr);
    if (resolved.has_value())
    {
        if (resolved->edit.has_value())
        {
            EncodeAcrossDocuments(resolved->edit.value());
        }
        return resolved.value();
    }
    return req;
}

lsp::requests::TextDocument_CodeLens::Result
Server::HandleRequestsTextDocument_CodeLens(lsp::requests::TextDocument_CodeLens::Params&& req)
{
    if (!m_config.features.enableCodeLens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    const std::string clientUri = req.textDocument.uri.toString();
    const auto occurrenceModule = OccurrenceModuleFor(doc->uri);
    features::CodeLensRequest clr{doc->uri,
                                  *doc->text,
                                  m_symbolTable,
                                  m_scopeIndex,
                                  m_logger.get(),
                                  m_config.info.predefinedFileExtension,
                                  &m_config.engineRules.unusedRules,
                                  m_i18n.get(),
                                  &m_config,
                                  clientUri,
                                  &occurrenceModule};
    auto lenses = features::GetCodeLenses(clr);
    if (lenses.has_value())
    {
        EncodeIn(*doc->text, lenses.value());
        return lenses.value();
    }
    return lsp::Null{};
}

lsp::requests::CodeLens_Resolve::Result
Server::HandleRequestsCodeLens_Resolve(lsp::requests::CodeLens_Resolve::Params&& req)
{
    if (!m_config.features.enableCodeLens)
    {
        return req;
    }

    features::CodeLensResolveRequest clrr{req, m_symbolTable, m_scopeIndex, m_logger.get()};
    auto resolved = features::ResolveCodeLens(clrr);
    return resolved.value_or(std::move(req));
}

} // namespace angel_lsp
