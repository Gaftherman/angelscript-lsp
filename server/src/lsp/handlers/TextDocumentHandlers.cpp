#include "lsp/Server.h"
#include "utils/Utils.h"
#include "utils/Timer.h"
#include "lsp/PositionCodec.h"
#include "features/hover/HoverHandler.h"
#include "features/definition/DefinitionHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/completion/CompletionHandler.h"
#include "features/signature_help/SignatureHelpHandler.h"
#include "features/document_symbol/DocumentSymbolHandler.h"
#include "features/references/ReferencesHandler.h"
#include "features/rename/RenameHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/formatting/FormattingHandler.h"
#include "features/document_link/DocumentLinkHandler.h"
#include "features/code_lens/CodeLensHandler.h"
#include "features/code_action/CodeActionHandler.h"
#include <spdlog/fmt/fmt.h>
#include <unordered_set>

namespace angel_lsp
{
    void Server::RegisterTextDocumentHandlers()
    {
        m_messageHandler->add<lsp::requests::TextDocument_Diagnostic>(
            [this](lsp::requests::TextDocument_Diagnostic::Params &&params)
            {
                return this->HandleRequestsTextDocument_Diagnostic(std::move(params));
            });

        m_messageHandler->add<lsp::requests::TextDocument_Hover>(
            [this](lsp::requests::TextDocument_Hover::Params &&req) -> lsp::requests::TextDocument_Hover::Result
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

                features::HoverRequest hr{
                    doc->uri,
                    *doc->text,
                    doc->tree,
                    m_symbolTable,
                    m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    [this](const std::string &uri) { return FindDocumentText(uri); },
                    &m_config,
                    [this, uriStr = doc->uri](const std::string &rawPath) {
                        return angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                            rawPath, CanonicalPathFromUri(uriStr), *SearchDirectories(),
                            IncludeAllowedRoots(), ImplicitIncludeExtension());
                    },
                    m_logger.get()
                };
                auto hover = features::GetHover(hr);
                double roundtripMs = roundtripTimer.ElapsedMs();
                LogInfo(fmt::format(
                    "[Hover Roundtrip] Total: {:.2f} ms for {} at {}:{}",
                    roundtripMs, doc->uri, req.position.line, req.position.character));

                if (hover.has_value())
                {
                    EncodeIn(*doc->text, hover.value());
                    return hover.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Definition>(
            [this](lsp::requests::TextDocument_Definition::Params &&req) -> lsp::requests::TextDocument_Definition::Result
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

                features::DefinitionRequest dr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                dr.resolveInclude = [this, uriStr = doc->uri](const std::string &rawPath) {
                    return angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                        rawPath, CanonicalPathFromUri(uriStr), *SearchDirectories(),
                        IncludeAllowedRoots(), ImplicitIncludeExtension());
                };
                auto defs = features::GetDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Moniker>(
            [this](lsp::requests::TextDocument_Moniker::Params &&req) -> lsp::requests::TextDocument_Moniker::Result
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

                features::DefinitionRequest dr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                dr.resolveInclude = [this, uriStr = doc->uri](const std::string &rawPath) {
                    return angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                        rawPath, CanonicalPathFromUri(uriStr), *SearchDirectories(),
                        IncludeAllowedRoots(), ImplicitIncludeExtension());
                };

                const auto defs = features::GetDefinition(dr);
                if (!defs.has_value() || defs->empty())
                {
                    return lsp::Null{};
                }

                const lsp::Location &where = (*defs)[0];
                const std::string declaredIn = DocumentKey(where.uri.toString());

                std::string qualified;
                m_symbolTable.ForEachSymbolInFile(declaredIn,
                    [&](const std::string &, const std::vector<angel_lsp::analysis::Symbol> &symbols)
                    {
                        for (const auto &sym : symbols)
                        {
                            if (sym.startLine != where.range.start.line || !qualified.empty())
                            {
                                continue;
                            }
                            qualified = sym.containerName.empty() ? sym.name
                                                                  : sym.containerName + "::" + sym.name;
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

                return lsp::Array<lsp::Moniker>{ moniker };
            });

        m_messageHandler->add<lsp::requests::TextDocument_InlineCompletion>(
            [](lsp::requests::TextDocument_InlineCompletion::Params &&) -> lsp::requests::TextDocument_InlineCompletion::Result
            {
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Declaration>(
            [this](lsp::requests::TextDocument_Declaration::Params &&req) -> lsp::requests::TextDocument_Declaration::Result
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

                features::DefinitionRequest dr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                auto defs = features::GetDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Implementation>(
            [this](lsp::requests::TextDocument_Implementation::Params &&req) -> lsp::requests::TextDocument_Implementation::Result
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

                features::ImplementationRequest ir{
                    doc->uri, *doc->text, doc->tree, m_symbolTable,
                    codec::Decode(*doc->text, m_positionEncoding, req.position), m_logger.get()
                };
                auto impls = features::GetImplementations(ir);
                if (impls.has_value() && !impls->empty())
                {
                    EncodeAcrossDocuments(impls.value());
                    return impls.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_TypeDefinition>(
            [this](lsp::requests::TextDocument_TypeDefinition::Params &&req) -> lsp::requests::TextDocument_TypeDefinition::Result
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

                features::DefinitionRequest dr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                auto defs = features::GetTypeDefinition(dr);
                if (defs.has_value() && !defs->empty())
                {
                    EncodeAcrossDocuments(defs.value());
                    return defs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Completion>(
            [this](lsp::requests::TextDocument_Completion::Params &&req) -> lsp::requests::TextDocument_Completion::Result
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
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position), &m_config, m_snippetSupport
                };

                cr.documentPath = CanonicalPathFromUri(doc->uri);
                cr.implicitExtension = std::string(ImplicitIncludeExtension());
                cr.listIncludeCandidates = [this]() { return IncludableFiles(); };

                return features::GetCompletion(cr);
            });

        m_messageHandler->add<lsp::requests::CompletionItem_Resolve>(
            [this](lsp::requests::CompletionItem_Resolve::Params &&req) -> lsp::requests::CompletionItem_Resolve::Result
            {
                if (!m_config.features.enableCompletion)
                {
                    return req;
                }

                features::CompletionResolveRequest rr{
                    req,
                    m_symbolTable,
                    [this](const std::string &uri) { return FindDocumentText(uri); }
                };
                return features::ResolveCompletionItem(rr);
            });

        m_messageHandler->add<lsp::requests::TextDocument_SignatureHelp>(
            [this](lsp::requests::TextDocument_SignatureHelp::Params &&req) -> lsp::requests::TextDocument_SignatureHelp::Result
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

                features::SignatureHelpRequest sr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                auto sig = features::GetSignatureHelp(sr);
                if (sig.has_value())
                {
                    return sig.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentSymbol>(
            [this](lsp::requests::TextDocument_DocumentSymbol::Params &&req) -> lsp::requests::TextDocument_DocumentSymbol::Result
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

                features::DocumentSymbolRequest dr{ doc->uri, *doc->text, doc->tree, m_symbolTable };
                auto symbols = features::GetDocumentSymbols(dr);
                if (symbols.has_value())
                {
                    EncodeIn(*doc->text, symbols.value());
                    return symbols.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_References>(
            [this](lsp::requests::TextDocument_References::Params &&req) -> lsp::requests::TextDocument_References::Result
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

                features::ReferencesRequest rr{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    req.context.includeDeclaration, m_symbolTable, m_scopeIndex, m_logger.get()
                };
                auto refs = features::GetReferences(rr);
                if (refs.has_value())
                {
                    EncodeAcrossDocuments(refs.value());
                    return refs.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_PrepareRename>(
            [this](lsp::requests::TextDocument_PrepareRename::Params &&req) -> lsp::requests::TextDocument_PrepareRename::Result
            {
                if (!m_config.features.enableRename)
                {
                    return lsp::Null{};
                }
                const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
                if (!doc)
                {
                    return lsp::Null{};
                }

                std::unordered_set<std::string> predefinedUris;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    predefinedUris.insert(m_predefinedUris.begin(), m_predefinedUris.end());
                }

                features::PrepareRenameRequest pr{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    m_symbolTable, m_scopeIndex, predefinedUris, m_logger.get()
                };
                auto prep = features::PrepareRename(pr);
                if (prep.has_value())
                {
                    EncodeIn(*doc->text, prep.value());
                    return prep.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_Rename>(
            [this](lsp::requests::TextDocument_Rename::Params &&req) -> lsp::requests::TextDocument_Rename::Result
            {
                if (!m_config.features.enableRename)
                {
                    return lsp::Null{};
                }
                const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
                if (!doc)
                {
                    return lsp::Null{};
                }

                std::unordered_set<std::string> predefinedUris;
                {
                    std::lock_guard<std::mutex> lock(m_predefinedMutex);
                    predefinedUris.insert(m_predefinedUris.begin(), m_predefinedUris.end());
                }

                features::RenameRequest rr{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    req.newName, m_symbolTable, m_scopeIndex, predefinedUris, m_logger.get()
                };
                auto edit = features::Rename(rr);
                if (edit.has_value())
                {
                    EncodeAcrossDocuments(edit.value());
                    return edit.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentHighlight>(
            [this](lsp::requests::TextDocument_DocumentHighlight::Params &&req) -> lsp::requests::TextDocument_DocumentHighlight::Result
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

                features::DocumentHighlightRequest hr{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    m_symbolTable, m_scopeIndex
                };
                auto highlights = features::GetDocumentHighlights(hr);
                if (highlights.has_value() && !highlights->empty())
                {
                    EncodeIn(*doc->text, highlights.value());
                    return highlights.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_CodeAction>(
            [this](lsp::requests::TextDocument_CodeAction::Params &&req) -> lsp::requests::TextDocument_CodeAction::Result
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
                for (auto &diag : context.diagnostics)
                {
                    diag.range = codec::Decode(*doc->text, m_positionEncoding, diag.range);
                }

                features::CodeActionRequest car{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.range),
                    context, m_symbolTable, m_scopeIndex
                };
                auto actions = features::GetCodeActions(car);
                if (actions.has_value())
                {
                    EncodeAcrossDocuments(*actions);
                    lsp::Array<lsp::OneOf<lsp::Command, lsp::CodeAction>> resultList;
                    resultList.reserve(actions->size());
                    for (auto &action : *actions)
                    {
                        resultList.push_back(std::move(action));
                    }
                    return resultList;
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::CodeAction_Resolve>(
            [this](lsp::requests::CodeAction_Resolve::Params &&req) -> lsp::requests::CodeAction_Resolve::Result
            {
                if (!m_config.features.enableCodeAction)
                {
                    return req;
                }

                features::CodeActionResolveRequest carr{ req, m_symbolTable, m_scopeIndex };
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
            });

        m_messageHandler->add<lsp::requests::TextDocument_Formatting>(
            [this](lsp::requests::TextDocument_Formatting::Params &&req) -> lsp::requests::TextDocument_Formatting::Result
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

                features::FormattingRequest fr{ doc->uri, *doc->text, doc->tree, req.options, CurrentBraceStyle() };
                auto edits = features::FormatDocument(fr);
                if (edits.has_value())
                {
                    EncodeIn(*doc->text, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_DocumentLink>(
            [this](lsp::requests::TextDocument_DocumentLink::Params &&req) -> lsp::requests::TextDocument_DocumentLink::Result
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

                features::DocumentLinkRequest dlr{ doc->uri, *doc->text, *searchDirectories, m_i18n.get(), IncludeAllowedRoots() };
                dlr.implicitExtension = std::string(ImplicitIncludeExtension());
                auto links = features::GetDocumentLinks(dlr);
                if (links.has_value())
                {
                    EncodeIn(*doc->text, links.value());
                    return links.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::DocumentLink_Resolve>(
            [this](lsp::requests::DocumentLink_Resolve::Params &&req) -> lsp::requests::DocumentLink_Resolve::Result
            {
                return this->HandleRequestsDocumentLink_Resolve(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_RangeFormatting>(
            [this](lsp::requests::TextDocument_RangeFormatting::Params &&req) -> lsp::requests::TextDocument_RangeFormatting::Result
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

                features::RangeFormattingRequest rfr{
                    doc->uri, *doc->text, doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.range),
                    req.options, CurrentBraceStyle()
                };
                auto edits = features::FormatRange(rfr);
                if (edits.has_value())
                {
                    EncodeIn(*doc->text, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::TextDocument_RangesFormatting>(
            [this](lsp::requests::TextDocument_RangesFormatting::Params &&req) -> lsp::requests::TextDocument_RangesFormatting::Result
            {
                return this->HandleRequestsTextDocument_RangesFormatting(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_CodeLens>(
            [this](lsp::requests::TextDocument_CodeLens::Params &&req) -> lsp::requests::TextDocument_CodeLens::Result
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

                features::CodeLensRequest clr{ doc->uri, *doc->text, doc->tree, m_symbolTable, m_scopeIndex, m_logger.get() };
                auto lenses = features::GetCodeLenses(clr);
                if (lenses.has_value())
                {
                    EncodeIn(*doc->text, lenses.value());
                    return lenses.value();
                }
                return lsp::Null{};
            });

        m_messageHandler->add<lsp::requests::CodeLens_Resolve>(
            [this](lsp::requests::CodeLens_Resolve::Params &&req) -> lsp::requests::CodeLens_Resolve::Result
            {
                if (!m_config.features.enableCodeLens)
                {
                    return req;
                }

                features::CodeLensResolveRequest clrr{ req, m_symbolTable, m_scopeIndex, m_logger.get() };
                auto resolved = features::ResolveCodeLens(clrr);
                return resolved.value_or(std::move(req));
            });

        m_messageHandler->add<lsp::requests::TextDocument_OnTypeFormatting>(
            [this](lsp::requests::TextDocument_OnTypeFormatting::Params &&req) -> lsp::requests::TextDocument_OnTypeFormatting::Result
            {
                if (!m_config.features.enableOnTypeFormatting)
                {
                    return lsp::Null{};
                }
                const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
                if (!doc)
                {
                    return lsp::Null{};
                }

                features::OnTypeFormattingRequest otfr{
                    doc->uri,
                    *doc->text,
                    doc->tree,
                    codec::Decode(*doc->text, m_positionEncoding, req.position),
                    req.ch,
                    req.options,
                    CurrentBraceStyle()
                };
                auto edits = features::FormatOnType(otfr);
                if (edits.has_value())
                {
                    EncodeIn(*doc->text, edits.value());
                    return edits.value();
                }
                return lsp::Null{};
            });
    }
}
