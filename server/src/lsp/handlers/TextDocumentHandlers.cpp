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
#include <fstream>

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

    lsp::requests::TextDocument_Diagnostic::Result Server::HandleRequestsTextDocument_Diagnostic(lsp::requests::TextDocument_Diagnostic::Params &&params)
    {
        if (!m_config.features.enablePullDiagnostics)
        {
            throw lsp::RequestError(lsp::MessageError::MethodNotFound, "Pull diagnostics are disabled");
        }

        const std::string uriStr = DocumentKey(params.textDocument.uri.toString());

        // The cached answer is only usable while it still describes the text the client is asking
        // about. This used to check only that an entry existed, which is true from the first
        // analysis onward, so every later pull was answered from whatever had been computed before
        // the edit in hand. The editor renders push and pull as two separate collections, so the
        // stale pull answer sat beside the correct push one and only cleared on the next keystroke.
        const std::string *current = FindDocumentText(uriStr);
        const size_t currentHash = current ? std::hash<std::string>{}(*current) : 0;
        const int currentVersion = GetDocumentVersion(uriStr);

        if (current)
        {
            std::lock_guard<std::mutex> lock(m_diagnosticsCacheMutex);
            if (const auto it = m_diagnosticsCache.find(uriStr);
                it != m_diagnosticsCache.end())
            {
                bool isCurrent = false;
                if (currentVersion >= 0 && it->second.version >= 0)
                {
                    isCurrent = (it->second.version == currentVersion);
                }
                else
                {
                    isCurrent = (it->second.textHash == currentHash);
                }

                if (isCurrent)
                {
                    if (params.previousResultId.has_value() && *params.previousResultId == it->second.resultId)
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
            }
        }

        // Nothing analysed yet, or nothing analysed for *this* text. Queue it and tell the client to
        // ask again rather than answering with an empty report - an empty report says "this file is
        // clean", which is a claim this server is in no position to make about a document it has not
        // looked at - and rather than answering with the previous one, which says something worse:
        // that a mistake the user has already corrected is still there.
        if (const std::string *text = FindDocumentText(uriStr))
            ScheduleAnalysis(uriStr, *text);

        lsp::json::Object retrigger;
        retrigger["retriggerRequest"] = true;

        throw lsp::RequestError(lsp::MessageError::ServerCancelled,
                                "Diagnostics for this document are still being computed",
                                lsp::json::Value(std::move(retrigger)));
    }


    lsp::requests::DocumentLink_Resolve::Result Server::HandleRequestsDocumentLink_Resolve(lsp::requests::DocumentLink_Resolve::Params &&params)
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


    lsp::requests::InlayHint_Resolve::Result Server::HandleRequestsInlayHint_Resolve(lsp::requests::InlayHint_Resolve::Params &&params)
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


    lsp::requests::TextDocument_RangesFormatting::Result Server::HandleRequestsTextDocument_RangesFormatting(lsp::requests::TextDocument_RangesFormatting::Params &&params)
    {
        if (!m_config.features.enableFormatting)
        {
            return lsp::Null{};
        }
        const auto doc = LookupOpenDocument(params.textDocument.uri.toString());
        if (!doc)
            return lsp::Null{};

        std::vector<lsp::TextEdit> allEdits;
        for (const auto &range : params.ranges)
        {
            features::RangeFormattingRequest rfr{
                doc->uri,
                *doc->text,
                doc->tree,
                codec::Decode(*doc->text, m_positionEncoding, range),
                params.options,
                CurrentBraceStyle()
            };
            auto edits = features::FormatRange(rfr);
            if (edits.has_value())
            {
                allEdits.insert(allEdits.end(),
                                std::make_move_iterator(edits->begin()),
                                std::make_move_iterator(edits->end()));
            }
        }
        EncodeIn(*doc->text, allEdits);
        return allEdits;
    }


    lsp::requests::TextDocument_WillSaveWaitUntil::Result Server::HandleRequestsTextDocument_WillSaveWaitUntil(lsp::requests::TextDocument_WillSaveWaitUntil::Params &&params)
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

        features::FormattingRequest fr{ doc->uri, *doc->text, doc->tree, options, CurrentBraceStyle() };
        auto edits = features::FormatDocument(fr);
        if (edits.has_value())
        {
            EncodeIn(*doc->text, edits.value());
            return edits.value();
        }

        return lsp::Array<lsp::TextEdit>{};
    }


    std::string Server::GenerateVirtualMixinDocument(std::string_view uri)
    {
        // Expected format: angelscript-virtual://<host_class>/<mixin>.as
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

        std::string hostClass;
        std::string mixinName;
        auto slashPos = s.find('/');
        if (slashPos != std::string_view::npos)
        {
            hostClass = angel_lsp::utils::UrlDecode(s.substr(0, slashPos));
            std::string_view mixinPart = s.substr(slashPos + 1);
            if (mixinPart.ends_with(".as"))
            {
                mixinPart.remove_suffix(3);
            }
            mixinName = angel_lsp::utils::UrlDecode(mixinPart);
        }
        else
        {
            std::string_view mixinPart = s;
            if (mixinPart.ends_with(".as"))
            {
                mixinPart.remove_suffix(3);
            }
            mixinName = angel_lsp::utils::UrlDecode(mixinPart);
        }

        std::string mixinPhysicalFileUri;
        uint32_t mixinStartLine = 0;
        uint32_t mixinEndLine = 0;
        bool foundMixin = false;
        std::string resolvedBaseClass;

        const angel_lsp::analysis::Symbol *mixinSym = nullptr;
        auto candidates = m_symbolTable.FindSymbols(mixinName);
        for (const auto &cand : candidates)
        {
            if (cand.type == angel_lsp::analysis::SymbolType::Class)
            {
                mixinSym = &cand;
                break;
            }
        }

        if (!mixinSym)
        {
            std::string shortName = mixinName;
            auto lastScope = shortName.rfind("::");
            if (lastScope != std::string::npos)
            {
                shortName = shortName.substr(lastScope + 2);
            }
            auto shortCandidates = m_symbolTable.FindTypeSymbolsByShortName(shortName);
            for (const auto &cand : shortCandidates)
            {
                if (cand.type == angel_lsp::analysis::SymbolType::Class)
                {
                    mixinSym = &cand;
                    break;
                }
            }
        }

        if (mixinSym)
        {
            mixinPhysicalFileUri = mixinSym->fileUri;
            mixinStartLine = mixinSym->startLine;
            mixinEndLine = mixinSym->fullRange.endLine > 0 ? mixinSym->fullRange.endLine : mixinSym->endLine;
            foundMixin = true;

            if (std::holds_alternative<angel_lsp::analysis::ClassSignature>(mixinSym->signature))
            {
                const auto &clsSig = std::get<angel_lsp::analysis::ClassSignature>(mixinSym->signature);
                if (!clsSig.bases.empty())
                {
                    resolvedBaseClass = fmt::format(" | Base: {}", fmt::join(clsSig.bases, ", "));
                }
            }
        }

        // Construct header (kVirtualMixinHeaderLineCount lines: 0..kVirtualMixinHeaderLineCount-1)
        static_assert(angel_lsp::analysis::SymbolTable::kVirtualMixinHeaderLineCount == 3);
        std::string result;
        result += fmt::format("// Virtual expanded mixin {} for host class {}\n", mixinName, hostClass);
        result += fmt::format("// Origin: {}{}\n", mixinPhysicalFileUri, resolvedBaseClass);
        result += "\n";

        std::string sourceText;
        if (!mixinPhysicalFileUri.empty())
        {
            if (const std::string *docText = FindDocumentText(mixinPhysicalFileUri))
            {
                sourceText = *docText;
            }
            else
            {
                std::string filePath = angel_lsp::utils::UriToPath(mixinPhysicalFileUri);
                std::ifstream file(filePath, std::ios::binary);
                if (file.is_open())
                {
                    sourceText.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                }
            }
        }

        if (foundMixin && !sourceText.empty())
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

            if (mixinStartLine < lines.size())
            {
                size_t effectiveEndLine = std::max<size_t>(mixinStartLine, mixinEndLine);
                size_t clampEnd = std::min<size_t>(effectiveEndLine, lines.size() - 1);
                for (size_t i = mixinStartLine; i <= clampEnd; ++i)
                {
                    result.append(lines[i]);
                    result.push_back('\n');
                }
            }
        }
        else
        {
            // Fallback synthesis if physical source is unavailable
            result += fmt::format("mixin class {}\n{{\n", mixinName);
            for (const auto &cand : candidates)
            {
                if (cand.type == angel_lsp::analysis::SymbolType::Function && cand.containerName == mixinName)
                {
                    result += fmt::format("    void {}();\n", cand.name);
                }
            }
            result += "}\n";
        }

        return result;
    }


    lsp::json::Value Server::HandleRequestsVirtualDocumentContent(lsp::json::Value &&params)
    {
        std::string uriStr;
        if (params.isObject())
        {
            const auto &obj = params.object();
            if (const auto *val = obj.find("uri"); val && val->isString())
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


}
