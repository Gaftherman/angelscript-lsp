#include "features/formatting/FormattingHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"

#include <iterator>
#include <utility>
#include <vector>

namespace angel_lsp
{

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
                                   features::ToLayoutOptions(req.options),
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
                                         codec::Decode(*doc->text, GetPositionEncoding(), req.range),
                                         features::ToLayoutOptions(req.options),
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

    const auto syntax =
        doc->tree ? ResolveProtocolSyntax(doc->uri, doc->docHandle->ParsedSource(), doc->docHandle.get()).syntax
                  : nullptr;
    features::OnTypeFormattingRequest otfr{doc->uri,
                                           *doc->text,
                                           syntax ? &syntax->index : nullptr,
                                           codec::Decode(*doc->text, GetPositionEncoding(), req.position),
                                           req.ch,
                                           features::ToLayoutOptions(req.options),
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
                                             codec::Decode(*doc->text, GetPositionEncoding(), range),
                                             features::ToLayoutOptions(params.options),
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
                                   features::ToLayoutOptions(options),
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

} // namespace angel_lsp
