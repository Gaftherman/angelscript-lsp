#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"

#include <optional>
#include <utility>
#include <vector>

namespace angel_lsp
{
/**
 * @brief Encodes hierarchy results and omits items whose declaration source is missing.
 * @param[in] items Byte-column results from a hierarchy feature.
 * @param[in] encode Encodes an item and reports whether it can be returned.
 * @return Encoded results, or null when no items remain.
 */
template <typename Item, typename Encode>
static lsp::NullOr<lsp::Array<Item>> EncodeHierarchyResult(std::optional<std::vector<Item>> items, Encode encode)
{
    if (!items)
    {
        return lsp::Null{};
    }
    auto output = items->begin();
    for (auto item = items->begin(); item != items->end(); ++item)
    {
        if (encode(*item))
        {
            if (output != item)
            {
                *output = std::move(*item);
            }
            ++output;
        }
    }
    items->erase(output, items->end());
    if (items->empty())
    {
        return lsp::Null{};
    }
    return std::move(*items);
}

template <typename Item>
static lsp::NullOr<lsp::Array<Item>> EncodeHierarchyItems(const Server& server, std::optional<std::vector<Item>> items)
{
    return EncodeHierarchyResult(std::move(items), [&server](Item& item) { return server.TryEncodeItemRanges(item); });
}

static lsp::requests::TextDocument_PrepareCallHierarchy::Result
PrepareCallHierarchy(Server& server, lsp::requests::TextDocument_PrepareCallHierarchy::Params&& req)
{
    if (!server.GetConfig().features.enableCallHierarchy)
    {
        return lsp::Null{};
    }
    const auto doc = server.LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::CallHierarchyPrepareRequest pr{doc->uri, *doc->text, server.GetSymbolTable(),
                                             codec::Decode(*doc->text, server.GetPositionEncoding(), req.position)};
    return EncodeHierarchyItems(server, features::PrepareCallHierarchy(pr));
}

static lsp::requests::CallHierarchy_IncomingCalls::Result
GetIncomingCalls(Server& server, lsp::requests::CallHierarchy_IncomingCalls::Params&& req)
{
    if (!server.GetConfig().features.enableCallHierarchy)
    {
        return lsp::Null{};
    }

    features::CallHierarchyItemRequest ir{server.GetSymbolTable(), server.GetCallGraph(), req.item};
    const auto encode = [&server](lsp::CallHierarchyIncomingCall& call)
    {
        if (!server.TryEncodeItemRanges(call.from))
        {
            return false;
        }
        server.EncodeRangesIn(call.from.uri.toString(), call.fromRanges);
        return true;
    };
    return EncodeHierarchyResult(features::GetIncomingCalls(ir), encode);
}

static lsp::requests::CallHierarchy_OutgoingCalls::Result
GetOutgoingCalls(Server& server, lsp::requests::CallHierarchy_OutgoingCalls::Params&& req)
{
    if (!server.GetConfig().features.enableCallHierarchy)
    {
        return lsp::Null{};
    }

    const std::string callerUri = req.item.uri.toString();

    features::CallHierarchyItemRequest ir{server.GetSymbolTable(), server.GetCallGraph(), req.item};
    const auto encode = [&server, &callerUri](lsp::CallHierarchyOutgoingCall& call)
    {
        if (!server.TryEncodeItemRanges(call.to))
        {
            return false;
        }
        server.EncodeRangesIn(callerUri, call.fromRanges);
        return true;
    };
    return EncodeHierarchyResult(features::GetOutgoingCalls(ir), encode);
}

lsp::requests::TextDocument_PrepareTypeHierarchy::Result
Server::PrepareTypeHierarchy(lsp::requests::TextDocument_PrepareTypeHierarchy::Params&& req)
{
    if (!GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc || !doc->docHandle || !doc->text)
    {
        return lsp::Null{};
    }

    const std::string_view source = *doc->text;
    const auto parsedSource = doc->predefinedHandle ? source : doc->docHandle->ParsedSource();
    const auto syntax = ResolveProtocolSyntax(doc->uri, parsedSource, doc->docHandle.get()).syntax;
    if (!syntax)
    {
        return lsp::Null{};
    }

    const auto encoding = GetPositionEncoding();
    const features::TypeHierarchyPrepareRequest pr{doc->uri,      source,       parsedSource, GetSymbolTable(),
                                                   syntax->index, req.position, encoding};
    auto items = features::PrepareTypeHierarchy(pr);
    if (!items.has_value() || items->empty())
    {
        return lsp::Null{};
    }
    auto& result = items.value();
    for (auto item = result.begin(); item != result.end();)
    {
        bool encoded = true;
        if (DocumentKey(item->uri.toString()) == doc->uri)
        {
            codec::Encode(syntax->index.Lines(), source, encoding, item->range);
            codec::Encode(syntax->index.Lines(), source, encoding, item->selectionRange);
        }
        else
        {
            encoded = TryEncodeItemRanges(*item);
        }
        item = encoded ? item + 1 : result.erase(item);
    }
    if (result.empty())
    {
        return lsp::Null{};
    }
    return result;
}

static lsp::requests::TypeHierarchy_Supertypes::Result
GetSupertypes(Server& server, lsp::requests::TypeHierarchy_Supertypes::Params&& req)
{
    if (!server.GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }

    features::TypeHierarchyItemRequest ir{server.GetSymbolTable(), req.item};
    return EncodeHierarchyItems(server, features::GetSupertypes(ir));
}

static lsp::requests::TypeHierarchy_Subtypes::Result GetSubtypes(Server& server,
                                                                 lsp::requests::TypeHierarchy_Subtypes::Params&& req)
{
    if (!server.GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }

    features::TypeHierarchyItemRequest ir{server.GetSymbolTable(), req.item};
    return EncodeHierarchyItems(server, features::GetSubtypes(ir));
}

void Server::RegisterHierarchyHandlers()
{
    m_messageHandler->add<lsp::requests::TextDocument_PrepareCallHierarchy>(
        [this](lsp::requests::TextDocument_PrepareCallHierarchy::Params&& req)
        { return PrepareCallHierarchy(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::CallHierarchy_IncomingCalls>(
        [this](lsp::requests::CallHierarchy_IncomingCalls::Params&& req)
        { return GetIncomingCalls(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::CallHierarchy_OutgoingCalls>(
        [this](lsp::requests::CallHierarchy_OutgoingCalls::Params&& req)
        { return GetOutgoingCalls(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TextDocument_PrepareTypeHierarchy>(
        [this](lsp::requests::TextDocument_PrepareTypeHierarchy::Params&& req)
        { return PrepareTypeHierarchy(std::move(req)); });

    m_messageHandler->add<lsp::requests::TypeHierarchy_Supertypes>(
        [this](lsp::requests::TypeHierarchy_Supertypes::Params&& req) { return GetSupertypes(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TypeHierarchy_Subtypes>(
        [this](lsp::requests::TypeHierarchy_Subtypes::Params&& req) { return GetSubtypes(*this, std::move(req)); });
}
} // namespace angel_lsp
