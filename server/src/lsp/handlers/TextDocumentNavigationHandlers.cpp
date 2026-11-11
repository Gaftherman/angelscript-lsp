#include "features/common/FeatureSharedUtils.h"
#include "features/definition/DefinitionHandler.h"
#include "features/document_highlight/DocumentHighlightHandler.h"
#include "features/implementation/ImplementationHandler.h"
#include "features/references/ReferencesHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/ScopedNameUtils.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <spdlog/fmt/fmt.h>

namespace angel_lsp
{

std::optional<std::string> Server::OccurrenceModuleFor(const std::string& uri) const
{
    if (m_modules.empty())
    {
        return std::nullopt;
    }
    const auto claim = ClaimFor(CanonicalPathFromUri(uri));
    return claim.owner ? std::optional(claim.owner->name) : std::nullopt;
}

static std::optional<analysis::CallBindingRevision> OpenCallBindingRevision(const OpenDocument& doc,
                                                                            uint64_t configRevision)
{
    if (!doc.docHandle || doc.docHandle->generation == 0)
    {
        return std::nullopt;
    }
    return analysis::CallBindingRevision{doc.docHandle->generation, doc.docHandle->version, configRevision};
}

features::DefinitionRequest Server::MakeDefinitionRequest(const OpenDocument& doc, const lsp::Position& position)
{
    const lsp::Position decoded = codec::Decode(*doc.text, GetPositionEncoding(), position);
    features::DefinitionRequest dr{
        .query =
            {
                .uri = doc.uri,
                .sourceCode = *doc.text,
                .symbolTable = m_symbolTable,
                .scopeIndex = m_scopeIndex,
                .line = decoded.line,
                .character = decoded.character,
                .predefinedExtension = m_config.info.predefinedFileExtension,
                .engineRules = &m_config.engineRules,
                .callBindingRevision = OpenCallBindingRevision(doc, m_sessionCoordinator->GetConfigRevision()),
            },
        .resolveInclude = [this, uri = doc.uri](std::string_view rawPath) -> std::optional<lsp::DocumentUri>
        {
            const std::string resolved =
                angel_lsp::utils::IncludeResolver::ResolveIncludePath(angel_lsp::utils::IncludeResolveRequest{
                    .includePath = rawPath,
                    .currentFilePath = CanonicalPathFromUri(uri),
                    .searchDirectories = *SearchDirectories(),
                    .allowedRoots = IncludeAllowedRoots(),
                    .implicitExtension = ImplicitIncludeExtension(),
                });
            if (resolved.empty())
            {
                return std::nullopt;
            }
            return features::common::ParseOrConvertUri(resolved);
        },
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

    features::ImplementationRequest ir{doc->uri, *doc->text, m_symbolTable,
                                       codec::Decode(*doc->text, GetPositionEncoding(), req.position), m_logger.get()};
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

    const auto occurrenceModule = OccurrenceModuleFor(doc->uri);
    features::ReferencesRequest rr{doc->uri,
                                   *doc->text,
                                   codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                   req.context.includeDeclaration,
                                   m_symbolTable,
                                   m_scopeIndex,
                                   m_logger.get(),
                                   OpenCallBindingRevision(*doc, m_sessionCoordinator->GetConfigRevision()),
                                   occurrenceModule ? std::optional<std::string_view>(*occurrenceModule)
                                                    : std::nullopt};
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

    features::DocumentHighlightRequest hr{doc->uri, *doc->text,
                                          codec::Decode(*doc->text, GetPositionEncoding(), req.position), m_symbolTable,
                                          m_scopeIndex};
    auto highlights = features::GetDocumentHighlights(hr);
    if (highlights.has_value() && !highlights->empty())
    {
        EncodeIn(*doc->text, highlights.value());
        return highlights.value();
    }
    return lsp::Null{};
}

} // namespace angel_lsp
