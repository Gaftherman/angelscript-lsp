#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"

namespace angel_lsp
{
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
    auto items = features::PrepareCallHierarchy(pr);
    if (!items.has_value() || items->empty())
    {
        return lsp::Null{};
    }
    for (auto& item : items.value())
    {
        server.EncodeItemRanges(item);
    }
    return items.value();
}

static lsp::requests::CallHierarchy_IncomingCalls::Result
GetIncomingCalls(Server& server, lsp::requests::CallHierarchy_IncomingCalls::Params&& req)
{
    if (!server.GetConfig().features.enableCallHierarchy)
    {
        return lsp::Null{};
    }

    features::CallHierarchyItemRequest ir{server.GetSymbolTable(), server.GetCallGraph(), req.item};
    auto calls = features::GetIncomingCalls(ir);
    if (!calls.has_value() || calls->empty())
    {
        return lsp::Null{};
    }
    for (auto& call : calls.value())
    {
        server.EncodeItemRanges(call.from);
        server.EncodeRangesIn(Server::DocumentKey(call.from.uri.toString()), call.fromRanges);
    }
    return calls.value();
}

static lsp::requests::CallHierarchy_OutgoingCalls::Result
GetOutgoingCalls(Server& server, lsp::requests::CallHierarchy_OutgoingCalls::Params&& req)
{
    if (!server.GetConfig().features.enableCallHierarchy)
    {
        return lsp::Null{};
    }

    const std::string callerUri = Server::DocumentKey(req.item.uri.toString());

    features::CallHierarchyItemRequest ir{server.GetSymbolTable(), server.GetCallGraph(), req.item};
    auto calls = features::GetOutgoingCalls(ir);
    if (!calls.has_value() || calls->empty())
    {
        return lsp::Null{};
    }
    for (auto& call : calls.value())
    {
        server.EncodeItemRanges(call.to);
        server.EncodeRangesIn(callerUri, call.fromRanges);
    }
    return calls.value();
}

static lsp::requests::TextDocument_PrepareTypeHierarchy::Result
PrepareTypeHierarchy(Server& server, lsp::requests::TextDocument_PrepareTypeHierarchy::Params&& req)
{
    if (!server.GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }
    const auto doc = server.LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    features::TypeHierarchyPrepareRequest pr{doc->uri, *doc->text, server.GetSymbolTable(),
                                             codec::Decode(*doc->text, server.GetPositionEncoding(), req.position)};
    auto items = features::PrepareTypeHierarchy(pr);
    if (!items.has_value() || items->empty())
    {
        return lsp::Null{};
    }
    for (auto& item : items.value())
    {
        server.EncodeItemRanges(item);
    }
    return items.value();
}

static lsp::requests::TypeHierarchy_Supertypes::Result
GetSupertypes(Server& server, lsp::requests::TypeHierarchy_Supertypes::Params&& req)
{
    if (!server.GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }

    features::TypeHierarchyItemRequest ir{server.GetSymbolTable(), req.item};
    auto items = features::GetSupertypes(ir);
    if (!items.has_value() || items->empty())
    {
        return lsp::Null{};
    }
    for (auto& item : items.value())
    {
        server.EncodeItemRanges(item);
    }
    return items.value();
}

static lsp::requests::TypeHierarchy_Subtypes::Result GetSubtypes(Server& server,
                                                                 lsp::requests::TypeHierarchy_Subtypes::Params&& req)
{
    if (!server.GetConfig().features.enableTypeHierarchy)
    {
        return lsp::Null{};
    }

    features::TypeHierarchyItemRequest ir{server.GetSymbolTable(), req.item};
    auto items = features::GetSubtypes(ir);
    if (!items.has_value() || items->empty())
    {
        return lsp::Null{};
    }
    for (auto& item : items.value())
    {
        server.EncodeItemRanges(item);
    }
    return items.value();
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
        { return PrepareTypeHierarchy(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TypeHierarchy_Supertypes>(
        [this](lsp::requests::TypeHierarchy_Supertypes::Params&& req) { return GetSupertypes(*this, std::move(req)); });

    m_messageHandler->add<lsp::requests::TypeHierarchy_Subtypes>(
        [this](lsp::requests::TypeHierarchy_Subtypes::Params&& req) { return GetSubtypes(*this, std::move(req)); });
}
} // namespace angel_lsp
