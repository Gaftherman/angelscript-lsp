#include "analysis/resolution/SemanticHelpers.h"
#include "features/code_action/CodeActionHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "features/hover/HoverHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <fstream>
#include <spdlog/fmt/fmt.h>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace angel_lsp
{
lsp::requests::TextDocument_Hover::Result
Server::HandleRequestsTextDocument_Hover(lsp::requests::TextDocument_Hover::Params&& req)
{
    utils::HighResTimer roundtripTimer;
    if (!m_config.features.enableHover)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    auto heldDocs = std::make_shared<std::vector<std::shared_ptr<const std::string>>>();
    features::HoverRequest hr{doc->uri,
                              *doc->text,
                              doc->tree,
                              m_symbolTable,
                              m_scopeIndex,
                              codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                              [this, heldDocs](const std::string& uri) -> const std::string*
                              {
                                  auto sp = FindDocumentText(uri);
                                  if (!sp)
                                  {
                                      return nullptr;
                                  }
                                  heldDocs->push_back(sp);
                                  return sp.get();
                              },
                              &m_config,
                              [this, uriStr = doc->uri](const std::string& rawPath)
                              {
                                  return angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                                      angel_lsp::utils::IncludeResolveRequest{
                                          .includePath = rawPath,
                                          .currentFilePath = CanonicalPathFromUri(uriStr),
                                          .searchDirectories = *SearchDirectories(),
                                          .allowedRoots = IncludeAllowedRoots(),
                                          .implicitExtension = ImplicitIncludeExtension(),
                                      });
                              },
                              m_logger.get(),
                              m_i18n.get()};
    auto hover = features::GetHover(hr);
    double roundtripMs = roundtripTimer.ElapsedMs();
    LogInfo(fmt::format("[Hover Roundtrip] Total: {:.2f} ms for {} at {}:{}", roundtripMs, doc->uri, req.position.line,
                        req.position.character));

    if (hover.has_value())
    {
        EncodeIn(*doc->text, hover.value());
        return hover.value();
    }
    return lsp::Null{};
}

features::DefinitionRequest Server::MakeDefinitionRequest(const OpenDocument& doc, const lsp::Position& position)
{
    features::DefinitionRequest dr{doc.uri,      *doc.text,
                                   doc.tree,     m_symbolTable,
                                   m_scopeIndex, codec::Decode(*doc.text, GetPositionEncoding(), position)};
    dr.engineRules = &m_config.engineRules;
    dr.predefinedExtension = m_config.info.predefinedFileExtension;
    dr.resolveInclude = [this, uri = doc.uri](const std::string& rawPath)
    {
        return angel_lsp::utils::IncludeResolver::ResolveIncludePath(angel_lsp::utils::IncludeResolveRequest{
            .includePath = rawPath,
            .currentFilePath = CanonicalPathFromUri(uri),
            .searchDirectories = *SearchDirectories(),
            .allowedRoots = IncludeAllowedRoots(),
            .implicitExtension = ImplicitIncludeExtension(),
        });
    };
    return dr;
}

template <typename FeatureFn>
lsp::requests::TextDocument_Definition::Result
Server::DispatchDefinitionQuery(const std::string& uriStr, const lsp::Position& position, FeatureFn&& fn)
{
    if (!m_config.features.enableDefinition)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(uriStr);
    if (!doc)
    {
        return lsp::Null{};
    }
    const auto dr = MakeDefinitionRequest(*doc, position);
    auto defs = fn(dr);
    if (defs.has_value() && !defs->empty())
    {
        EncodeAcrossDocuments(defs.value());
        return defs.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_Definition::Result
Server::HandleRequestsTextDocument_Definition(lsp::requests::TextDocument_Definition::Params&& req)
{
    return DispatchDefinitionQuery(req.textDocument.uri.toString(), req.position,
                                   [](const features::DefinitionRequest& dr) { return features::GetDefinition(dr); });
}

lsp::requests::TextDocument_Moniker::Result
Server::HandleRequestsTextDocument_Moniker(lsp::requests::TextDocument_Moniker::Params&& req)
{
    if (!m_config.features.enableDefinition)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }
    const auto dr = MakeDefinitionRequest(*doc, req.position);
    const auto defs = features::GetDefinition(dr);
    if (!defs.has_value() || defs->empty())
    {
        return lsp::Null{};
    }

    const lsp::Location& where = (*defs)[0];
    const std::string declaredIn = DocumentKey(where.uri.toString());

    std::string qualified;
    m_symbolTable.ForEachSymbolInFile(
        declaredIn,
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<angel_lsp::analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.startLine != where.range.start.line || !qualified.empty())
                {
                    continue;
                }
                qualified = utils::JoinScopedName(sym.containerName, sym.name);
            }
        });

    if (qualified.empty())
    {
        return lsp::Null{};
    }

    lsp::Moniker moniker;
    moniker.scheme = "angelscript";
    moniker.identifier = qualified;
    moniker.unique = lsp::UniquenessLevel::Project;
    moniker.kind = lsp::MonikerKind::Export;

    return lsp::Array<lsp::Moniker>{moniker};
}

lsp::requests::TextDocument_Declaration::Result
Server::HandleRequestsTextDocument_Declaration(lsp::requests::TextDocument_Declaration::Params&& req)
{
    return DispatchDefinitionQuery(req.textDocument.uri.toString(), req.position,
                                   [](const features::DefinitionRequest& dr) { return features::GetDefinition(dr); });
}

lsp::requests::TextDocument_Implementation::Result
Server::HandleRequestsTextDocument_Implementation(lsp::requests::TextDocument_Implementation::Params&& req)
{
    if (!m_config.features.enableImplementation)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::ImplementationRequest ir{doc->uri,
                                       *doc->text,
                                       doc->tree,
                                       m_symbolTable,
                                       codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                       m_logger.get()};
    auto impls = features::GetImplementations(ir);
    if (impls.has_value() && !impls->empty())
    {
        EncodeAcrossDocuments(impls.value());
        return impls.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_TypeDefinition::Result
Server::HandleRequestsTextDocument_TypeDefinition(lsp::requests::TextDocument_TypeDefinition::Params&& req)
{
    return DispatchDefinitionQuery(req.textDocument.uri.toString(), req.position,
                                   [](const features::DefinitionRequest& dr)
                                   { return features::GetTypeDefinition(dr); });
}

lsp::requests::TextDocument_References::Result
Server::HandleRequestsTextDocument_References(lsp::requests::TextDocument_References::Params&& req)
{
    if (!m_config.features.enableReferences)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::ReferencesRequest rr{doc->uri,
                                   *doc->text,
                                   doc->tree,
                                   codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                   req.context.includeDeclaration,
                                   m_symbolTable,
                                   m_scopeIndex,
                                   m_logger.get()};
    auto refs = features::GetReferences(rr);
    if (refs.has_value())
    {
        EncodeAcrossDocuments(refs.value());
        return refs.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_DocumentHighlight::Result
Server::HandleRequestsTextDocument_DocumentHighlight(lsp::requests::TextDocument_DocumentHighlight::Params&& req)
{
    if (!m_config.features.enableDocumentHighlight)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::DocumentHighlightRequest hr{doc->uri,      *doc->text,
                                          doc->tree,     codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                          m_symbolTable, m_scopeIndex};
    auto highlights = features::GetDocumentHighlights(hr);
    if (highlights.has_value() && !highlights->empty())
    {
        EncodeIn(*doc->text, highlights.value());
        return highlights.value();
    }
    return lsp::Null{};
}

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

    features::CompletionRequest cr{
        doc->uri,     *doc->text,
        doc->tree,    m_symbolTable,
        m_scopeIndex, codec::Decode(*doc->text, m_sessionCoordinator->GetPositionEncoding(), req.position),
        &m_config,    m_sessionCoordinator->GetClientSupportsSnippets()};

    cr.documentPath = CanonicalPathFromUri(doc->uri);
    cr.implicitExtension = std::string(ImplicitIncludeExtension());
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

    features::SignatureHelpRequest sr{doc->uri,     *doc->text,
                                      doc->tree,    m_symbolTable,
                                      m_scopeIndex, codec::Decode(*doc->text, GetPositionEncoding(), req.position)};
    auto sig = features::GetSignatureHelp(sr);
    if (sig.has_value())
    {
        return sig.value();
    }
    return lsp::Null{};
}

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

    features::PrepareRenameRequest pr{ctx->doc.uri,  *ctx->doc.text, ctx->doc.tree,       ctx->decodedPosition,
                                      m_symbolTable, m_scopeIndex,   ctx->predefinedUris, m_logger.get()};
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

    features::RenameRequest rr{ctx->doc.uri,  *ctx->doc.text, ctx->doc.tree,       ctx->decodedPosition, req.newName,
                               m_symbolTable, m_scopeIndex,   ctx->predefinedUris, m_logger.get()};
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

    lsp::CodeActionContext context = req.context;
    for (auto& diag : context.diagnostics)
    {
        diag.range = codec::Decode(*doc->text, GetPositionEncoding(), diag.range);
    }

    features::CodeActionRequest car{
        doc->uri,  *doc->text,    doc->tree,    codec::Decode(*doc->text, GetPositionEncoding(), req.range),
        context,   m_symbolTable, m_scopeIndex, IncludeAllowedRoots(),
        &m_config, m_i18n.get()};
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
    features::DocumentSymbolRequest dr{doc->uri, *doc->text, doc->tree, m_symbolTable};
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

lsp::requests::TextDocument_Formatting::Result
Server::HandleRequestsTextDocument_Formatting(lsp::requests::TextDocument_Formatting::Params&& req)
{
    if (!m_config.features.enableFormatting)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::FormattingRequest fr{doc->uri,
                                   *doc->text,
                                   doc->tree,
                                   req.options,
                                   CurrentBraceStyle(),
                                   CurrentSpacesInsideParentheses(),
                                   CurrentKeepEmptyBlocksOnSingleLine(),
                                   CurrentPointerAlignment()};
    auto edits = features::FormatDocument(fr);
    if (edits.has_value())
    {
        EncodeIn(*doc->text, edits.value());
        return edits.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_RangeFormatting::Result
Server::HandleRequestsTextDocument_RangeFormatting(lsp::requests::TextDocument_RangeFormatting::Params&& req)
{
    if (!m_config.features.enableFormatting)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::RangeFormattingRequest rfr{doc->uri,
                                         *doc->text,
                                         doc->tree,
                                         codec::Decode(*doc->text, GetPositionEncoding(), req.range),
                                         req.options,
                                         CurrentBraceStyle(),
                                         CurrentSpacesInsideParentheses(),
                                         CurrentKeepEmptyBlocksOnSingleLine(),
                                         CurrentPointerAlignment()};
    auto edits = features::FormatRange(rfr);
    if (edits.has_value())
    {
        EncodeIn(*doc->text, edits.value());
        return edits.value();
    }
    return lsp::Null{};
}

lsp::requests::TextDocument_OnTypeFormatting::Result
Server::HandleRequestsTextDocument_OnTypeFormatting(lsp::requests::TextDocument_OnTypeFormatting::Params&& req)
{
    if (!m_config.features.enableFormatting || !m_config.features.enableOnTypeFormatting)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::OnTypeFormattingRequest otfr{doc->uri,
                                           *doc->text,
                                           doc->tree,
                                           codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                           req.ch,
                                           req.options,
                                           CurrentBraceStyle(),
                                           CurrentSpacesInsideParentheses(),
                                           CurrentKeepEmptyBlocksOnSingleLine(),
                                           CurrentPointerAlignment()};
    auto edits = features::FormatOnType(otfr);
    if (edits.has_value())
    {
        EncodeIn(*doc->text, edits.value());
        return edits.value();
    }
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

    features::DocumentLinkRequest dlr{doc->uri, *doc->text, *searchDirectories, m_i18n.get()};
    dlr.allowedRoots = IncludeAllowedRoots();
    dlr.implicitExtension = std::string(ImplicitIncludeExtension());
    auto links = features::GetDocumentLinks(dlr);
    if (links.has_value())
    {
        EncodeIn(*doc->text, links.value());
        return links.value();
    }
    return lsp::Null{};
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

    features::CodeLensRequest clr{doc->uri,
                                  *doc->text,
                                  doc->tree,
                                  m_symbolTable,
                                  m_scopeIndex,
                                  m_logger.get(),
                                  m_config.info.predefinedFileExtension,
                                  &m_config.engineRules.unusedRules,
                                  m_i18n.get(),
                                  &m_config};
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

static void RegisterNavigationHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Hover>([&server](lsp::requests::TextDocument_Hover::Params&& req)
                                                   { return server.HandleRequestsTextDocument_Hover(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Definition>(
        [&server](lsp::requests::TextDocument_Definition::Params&& req)
        { return server.HandleRequestsTextDocument_Definition(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Moniker>(
        [&server](lsp::requests::TextDocument_Moniker::Params&& req)
        { return server.HandleRequestsTextDocument_Moniker(std::move(req)); });

    handler.add<lsp::requests::TextDocument_InlineCompletion>(
        [](lsp::requests::TextDocument_InlineCompletion::Params&&)
            -> lsp::requests::TextDocument_InlineCompletion::Result { return lsp::Null{}; });

    handler.add<lsp::requests::TextDocument_Declaration>(
        [&server](lsp::requests::TextDocument_Declaration::Params&& req)
        { return server.HandleRequestsTextDocument_Declaration(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Implementation>(
        [&server](lsp::requests::TextDocument_Implementation::Params&& req)
        { return server.HandleRequestsTextDocument_Implementation(std::move(req)); });

    handler.add<lsp::requests::TextDocument_TypeDefinition>(
        [&server](lsp::requests::TextDocument_TypeDefinition::Params&& req)
        { return server.HandleRequestsTextDocument_TypeDefinition(std::move(req)); });

    handler.add<lsp::requests::TextDocument_References>(
        [&server](lsp::requests::TextDocument_References::Params&& req)
        { return server.HandleRequestsTextDocument_References(std::move(req)); });

    handler.add<lsp::requests::TextDocument_DocumentHighlight>(
        [&server](lsp::requests::TextDocument_DocumentHighlight::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentHighlight(std::move(req)); });

    handler.add("textDocument/breakpointLocations", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_BreakpointLocations(std::move(params)); });

    handler.add("angelscript/breakpointLocations", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_BreakpointLocations(std::move(params)); });

    handler.add("textDocument/evaluatableExpression", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_EvaluatableExpression(std::move(params)); });

    handler.add("angelscript/evaluatableExpression", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsTextDocument_EvaluatableExpression(std::move(params)); });

    handler.add("angelscript/debugCompletion", [&server](lsp::json::Value&& params)
                { return server.HandleRequestsAngelscript_DebugCompletion(std::move(params)); });
}

static void RegisterEditingHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Completion>(
        [&server](lsp::requests::TextDocument_Completion::Params&& req)
        { return server.HandleRequestsTextDocument_Completion(std::move(req)); });

    handler.add<lsp::requests::CompletionItem_Resolve>(
        [&server](lsp::requests::CompletionItem_Resolve::Params&& req)
        { return server.HandleRequestsCompletionItem_Resolve(std::move(req)); });

    handler.add<lsp::requests::TextDocument_SignatureHelp>(
        [&server](lsp::requests::TextDocument_SignatureHelp::Params&& req)
        { return server.HandleRequestsTextDocument_SignatureHelp(std::move(req)); });

    handler.add<lsp::requests::TextDocument_PrepareRename>(
        [&server](lsp::requests::TextDocument_PrepareRename::Params&& req)
        { return server.HandleRequestsTextDocument_PrepareRename(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Rename>(
        [&server](lsp::requests::TextDocument_Rename::Params&& req)
        { return server.HandleRequestsTextDocument_Rename(std::move(req)); });

    handler.add<lsp::requests::TextDocument_CodeAction>(
        [&server](lsp::requests::TextDocument_CodeAction::Params&& req)
        { return server.HandleRequestsTextDocument_CodeAction(std::move(req)); });

    handler.add<lsp::requests::CodeAction_Resolve>([&server](lsp::requests::CodeAction_Resolve::Params&& req)
                                                   { return server.HandleRequestsCodeAction_Resolve(std::move(req)); });
}

static void RegisterFormattingAndSymbolHandlers(Server& server, lsp::MessageHandler& handler)
{
    handler.add<lsp::requests::TextDocument_Diagnostic>(
        [&server](lsp::requests::TextDocument_Diagnostic::Params&& params)
        { return server.HandleRequestsTextDocument_Diagnostic(std::move(params)); });

    handler.add<lsp::requests::TextDocument_DocumentSymbol>(
        [&server](lsp::requests::TextDocument_DocumentSymbol::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentSymbol(std::move(req)); });

    handler.add<lsp::requests::TextDocument_Formatting>(
        [&server](lsp::requests::TextDocument_Formatting::Params&& req)
        { return server.HandleRequestsTextDocument_Formatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_RangeFormatting>(
        [&server](lsp::requests::TextDocument_RangeFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_RangeFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_RangesFormatting>(
        [&server](lsp::requests::TextDocument_RangesFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_RangesFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_OnTypeFormatting>(
        [&server](lsp::requests::TextDocument_OnTypeFormatting::Params&& req)
        { return server.HandleRequestsTextDocument_OnTypeFormatting(std::move(req)); });

    handler.add<lsp::requests::TextDocument_DocumentLink>(
        [&server](lsp::requests::TextDocument_DocumentLink::Params&& req)
        { return server.HandleRequestsTextDocument_DocumentLink(std::move(req)); });

    handler.add<lsp::requests::DocumentLink_Resolve>(
        [&server](lsp::requests::DocumentLink_Resolve::Params&& req)
        { return server.HandleRequestsDocumentLink_Resolve(std::move(req)); });

    handler.add<lsp::requests::TextDocument_CodeLens>(
        [&server](lsp::requests::TextDocument_CodeLens::Params&& req)
        { return server.HandleRequestsTextDocument_CodeLens(std::move(req)); });

    handler.add<lsp::requests::CodeLens_Resolve>([&server](lsp::requests::CodeLens_Resolve::Params&& req)
                                                 { return server.HandleRequestsCodeLens_Resolve(std::move(req)); });
}

void Server::RegisterTextDocumentHandlers()
{
    RegisterNavigationHandlers(*this, *m_messageHandler);
    RegisterEditingHandlers(*this, *m_messageHandler);
    RegisterFormattingAndSymbolHandlers(*this, *m_messageHandler);
}

std::optional<lsp::requests::TextDocument_Diagnostic::Result>
Server::TryGetCachedDiagnosticReport(const std::string& uriStr, size_t currentHash, int currentVersion,
                                     const std::optional<std::string>& previousResultId)
{
    std::lock_guard<std::mutex> lock(m_diagnosticsPipeline->GetCacheMutexRef());
    const auto it = m_diagnosticsPipeline->GetCacheRef().find(uriStr);
    if (it == m_diagnosticsPipeline->GetCacheRef().end())
    {
        return std::nullopt;
    }

    const bool isCurrent = (it->second.configRevision == m_sessionCoordinator->GetConfigRevision()) &&
                           ((it->second.textHash == currentHash) ||
                            (currentVersion >= 0 && it->second.version >= 0 && it->second.version == currentVersion));
    if (!isCurrent)
    {
        return std::nullopt;
    }

    if (previousResultId.has_value() && *previousResultId == it->second.resultId)
    {
        lsp::RelatedUnchangedDocumentDiagnosticReport unchanged;
        unchanged.resultId = it->second.resultId;
        return unchanged;
    }

    lsp::RelatedFullDocumentDiagnosticReport full;
    full.resultId = it->second.resultId;
    full.items = it->second.items;
    return full;
}

lsp::requests::TextDocument_Diagnostic::Result
Server::HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params&& params)
{
    if (!m_config.features.enablePullDiagnostics)
    {
        throw lsp::RequestError(lsp::MessageError::MethodNotFound, "Pull diagnostics are disabled");
    }

    const std::string uriStr = DocumentKey(params.textDocument.uri.toString());

    if (!m_config.modules.empty() && !GetDocumentStore().IsOpen(uriStr))
    {
        const std::string path = CanonicalPathFromUri(uriStr);
        if (ClaimFor(path).owner == nullptr)
        {
            m_diagnosticsPipeline->InvalidateDiagnostics(uriStr);
            lsp::RelatedFullDocumentDiagnosticReport emptyReport;
            emptyReport.items = {};
            return emptyReport;
        }
    }

    const auto current = FindDocumentText(uriStr);
    const size_t currentHash = current ? std::hash<std::string>{}(*current) : 0;
    const int currentVersion = GetDocumentVersion(uriStr);

    if (current && m_parser)
    {
        if (auto cached = TryGetCachedDiagnosticReport(uriStr, currentHash, currentVersion, params.previousResultId))
        {
            return std::move(*cached);
        }

        AnalyzeDocument({
            .uriStr = uriStr,
            .text = *current,
            .parser = *m_parser,
            .version = currentVersion,
            .generation = GetDocumentStore().GetGeneration(uriStr),
            .configRevision = m_sessionCoordinator->GetConfigRevision(),
        });

        if (auto fresh = TryGetCachedDiagnosticReport(uriStr, currentHash, currentVersion, params.previousResultId))
        {
            return std::move(*fresh);
        }
    }

    lsp::json::Object retrigger;
    retrigger["retriggerRequest"] = true;

    throw lsp::RequestError(lsp::MessageError::ServerCancelled,
                            "Diagnostics for this document are still being computed",
                            lsp::json::Value(std::move(retrigger)));
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

lsp::requests::TextDocument_RangesFormatting::Result
Server::HandleRequestsTextDocument_RangesFormatting(lsp::requests::TextDocument_RangesFormatting::Params&& params)
{
    if (!m_config.features.enableFormatting)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(params.textDocument.uri.toString());
    if (!doc)
        return lsp::Null{};

    std::vector<lsp::TextEdit> allEdits;
    for (const auto& range : params.ranges)
    {
        features::RangeFormattingRequest rfr{doc->uri,
                                             *doc->text,
                                             doc->tree,
                                             codec::Decode(*doc->text, GetPositionEncoding(), range),
                                             params.options,
                                             CurrentBraceStyle(),
                                             CurrentSpacesInsideParentheses(),
                                             CurrentKeepEmptyBlocksOnSingleLine(),
                                             CurrentPointerAlignment()};
        auto edits = features::FormatRange(rfr);
        if (edits.has_value())
        {
            allEdits.insert(allEdits.end(), std::make_move_iterator(edits->begin()),
                            std::make_move_iterator(edits->end()));
        }
    }
    EncodeIn(*doc->text, allEdits);
    return allEdits;
}

lsp::requests::TextDocument_WillSaveWaitUntil::Result
Server::HandleRequestsTextDocument_WillSaveWaitUntil(lsp::requests::TextDocument_WillSaveWaitUntil::Params&& params)
{
    // Opt-in, and that is the load-bearing part. The editor has its own format-on-save
    // setting; a language server that reformats every manual save regardless would override a
    // choice the user made somewhere else, silently, on a file they were only trying to save.
    if (!m_config.features.enableFormatting || !m_config.format.formatOnSave)
    {
        return lsp::Array<lsp::TextEdit>{};
    }

    // A format triggered by an autosave timer rewrites the user's file while they are still
    // typing in it. Manual saves only, whatever the setting above says.
    if (params.reason != lsp::TextDocumentSaveReason::Manual)
    {
        return lsp::Array<lsp::TextEdit>{};
    }

    const auto doc = LookupOpenDocument(params.textDocument.uri.toString());
    if (!doc)
        return lsp::Array<lsp::TextEdit>{};

    lsp::FormattingOptions options;
    options.tabSize = 4;
    options.insertSpaces = true;

    features::FormattingRequest fr{doc->uri,
                                   *doc->text,
                                   doc->tree,
                                   options,
                                   CurrentBraceStyle(),
                                   CurrentSpacesInsideParentheses(),
                                   CurrentKeepEmptyBlocksOnSingleLine(),
                                   CurrentPointerAlignment()};
    auto edits = features::FormatDocument(fr);
    if (edits.has_value())
    {
        EncodeIn(*doc->text, edits.value());
        return edits.value();
    }

    return lsp::Array<lsp::TextEdit>{};
}

static VirtualMixinTarget ParseVirtualMixinUri(std::string_view uri)
{
    std::string_view s = uri;
    static constexpr std::string_view kVirtualSchemeFull = "angelscript-virtual://";
    static constexpr std::string_view kVirtualSchemeShort = "angelscript-virtual:";

    if (s.starts_with(kVirtualSchemeFull))
    {
        s.remove_prefix(kVirtualSchemeFull.size());
    }
    else if (s.starts_with(kVirtualSchemeShort))
    {
        s.remove_prefix(kVirtualSchemeShort.size());
        while (!s.empty() && s.front() == '/')
        {
            s.remove_prefix(1);
        }
    }

    VirtualMixinTarget target;
    auto slashPos = s.find('/');
    std::string_view mixinPart = (slashPos != std::string_view::npos) ? s.substr(slashPos + 1) : s;
    if (slashPos != std::string_view::npos)
    {
        target.hostClass = angel_lsp::utils::UrlDecode(s.substr(0, slashPos));
    }
    if (mixinPart.ends_with(".as"))
    {
        mixinPart.remove_suffix(3);
    }
    target.mixinName = angel_lsp::utils::UrlDecode(mixinPart);
    return target;
}

static const angel_lsp::analysis::Symbol* FindMixinClassSymbol(const angel_lsp::analysis::SymbolTable& symTable,
                                                               const std::string& mixinName,
                                                               std::vector<angel_lsp::analysis::Symbol>& outCandidates)
{
    outCandidates = symTable.FindSymbols(mixinName);
    for (const auto& cand : outCandidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Class)
        {
            return &cand;
        }
    }

    std::string shortName = std::string(angel_lsp::analysis::LastScopeSegment(mixinName));
    auto shortCandidates = symTable.FindTypeSymbolsByShortName(shortName);
    for (const auto& cand : shortCandidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Class)
        {
            return &cand;
        }
    }
    return nullptr;
}

static MixinSymbolLocation ResolveMixinLocation(const angel_lsp::analysis::Symbol* mixinSym)
{
    MixinSymbolLocation loc;
    if (!mixinSym)
    {
        return loc;
    }
    loc.fileUri = mixinSym->fileUri;
    loc.startLine = mixinSym->startLine;
    loc.endLine = mixinSym->fullRange.endLine > 0 ? mixinSym->fullRange.endLine : mixinSym->endLine;
    loc.found = true;

    if (std::holds_alternative<angel_lsp::analysis::ClassSignature>(mixinSym->signature))
    {
        const auto& clsSig = std::get<angel_lsp::analysis::ClassSignature>(mixinSym->signature);
        const std::vector<std::string> bases = clsSig.bases;
        if (!bases.empty())
        {
            loc.resolvedBaseClass = fmt::format(" | Base: {}", fmt::join(bases, ", "));
        }
    }
    return loc;
}

static void AppendMixinSourceLines(std::string_view sourceText, uint32_t startLine, uint32_t endLine, std::string& out)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start < sourceText.size())
    {
        size_t end = sourceText.find('\n', start);
        if (end == std::string_view::npos)
        {
            lines.push_back(std::string_view(sourceText).substr(start));
            break;
        }
        std::string_view line = std::string_view(sourceText).substr(start, end - start);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }

    if (startLine < lines.size())
    {
        size_t effectiveEndLine = std::max<size_t>(startLine, endLine);
        size_t clampEnd = std::min<size_t>(effectiveEndLine, lines.size() - 1);
        for (size_t i = startLine; i <= clampEnd; ++i)
        {
            out.append(lines[i]);
            out.push_back('\n');
        }
    }
}

static void AppendFallbackMixin(std::string_view mixinName, const std::vector<angel_lsp::analysis::Symbol>& candidates,
                                std::string& out)
{
    out += fmt::format("mixin class {}\n{{\n", mixinName);
    for (const auto& cand : candidates)
    {
        if (cand.type == angel_lsp::analysis::SymbolType::Function && cand.containerName == mixinName)
        {
            out += fmt::format("    void {}();\n", cand.name);
        }
    }
    out += "}\n";
}

std::string Server::GenerateVirtualMixinDocument(std::string_view uri)
{
    const auto target = ParseVirtualMixinUri(uri);
    std::vector<angel_lsp::analysis::Symbol> candidates;
    const auto* mixinSym = FindMixinClassSymbol(m_symbolTable, target.mixinName, candidates);
    const auto loc = ResolveMixinLocation(mixinSym);

    static_assert(angel_lsp::analysis::SymbolTable::kVirtualMixinHeaderLineCount == 3);
    std::string result;
    result += fmt::format("// Virtual expanded mixin {} for host class {}\n", target.mixinName, target.hostClass);
    result += fmt::format("// Origin: {}{}\n\n", loc.fileUri, loc.resolvedBaseClass);

    std::string sourceText;
    if (!loc.fileUri.empty())
    {
        if (auto docText = FindDocumentText(loc.fileUri))
        {
            sourceText = *docText;
        }
        else
        {
            std::string filePath = angel_lsp::utils::UriToPath(loc.fileUri);
            std::ifstream file(filePath, std::ios::binary);
            if (file.is_open())
            {
                std::ostringstream ss;
                ss << file.rdbuf();
                sourceText = ss.str();
            }
        }
    }

    if (loc.found && !sourceText.empty())
    {
        AppendMixinSourceLines(sourceText, loc.startLine, loc.endLine, result);
    }
    else
    {
        AppendFallbackMixin(target.mixinName, candidates, result);
    }

    return result;
}

lsp::json::Value Server::HandleRequestsVirtualDocumentContent(lsp::json::Value&& params)
{
    std::string uriStr;
    if (params.isObject())
    {
        const auto& obj = params.object();
        if (const auto* val = obj.find("uri"); val && val->isString())
        {
            uriStr = val->string();
        }
    }
    else if (params.isString())
    {
        uriStr = params.string();
    }

    if (uriStr.empty())
    {
        throw lsp::RequestError(lsp::MessageError::InvalidParams, "Missing 'uri' parameter");
    }

    std::string content = GenerateVirtualMixinDocument(uriStr);

    lsp::json::Object answer;
    answer["content"] = lsp::json::Value(std::move(content));
    return lsp::json::Value(std::move(answer));
}

} // namespace angel_lsp
