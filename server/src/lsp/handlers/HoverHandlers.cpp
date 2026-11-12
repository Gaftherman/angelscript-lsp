#include "features/hover/HoverHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/IncludeResolver.h"
#include "utils/Timer.h"

#include <memory>
#include <spdlog/fmt/fmt.h>
#include <string>
#include <string_view>
#include <utility>
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
    if (!doc || !doc->docHandle || !doc->docHandle->tree)
    {
        return lsp::Null{};
    }

    const auto syntax = ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax;
    if (!syntax)
    {
        return lsp::Null{};
    }

    auto heldDocs = std::make_shared<std::vector<std::shared_ptr<const std::string>>>();
    features::HoverRequest hr{.uri = doc->uri,
                              .sourceCode = *doc->text,
                              .nodeIndex = &syntax->index,
                              .symbolTable = m_symbolTable,
                              .scopeIndex = m_scopeIndex,
                              .position = codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                              .sources = analysis::HoverDocumentSources{
                                  [this, heldDocs](std::string_view uri) -> const std::string*
                                  {
                                      auto sp = FindDocumentText(std::string(uri));
                                      if (!sp)
                                      {
                                          return nullptr;
                                      }
                                      heldDocs->push_back(sp);
                                      return sp.get();
                                  },
                                  [this, uriStr = doc->uri](std::string_view rawPath)
                                  {
                                      return angel_lsp::utils::IncludeResolver::ResolveIncludePath(
                                          angel_lsp::utils::IncludeResolveRequest{
                                              .includePath = std::string(rawPath),
                                              .currentFilePath = CanonicalPathFromUri(uriStr),
                                              .searchDirectories = *SearchDirectories(),
                                              .allowedRoots = IncludeAllowedRoots(),
                                              .implicitExtension = ImplicitIncludeExtension(),
                                          });
                                  },
                              },
                              .config = &m_config,
                              .logger = m_logger.get(),
                              .i18n = m_i18n.get()};
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

} // namespace angel_lsp
