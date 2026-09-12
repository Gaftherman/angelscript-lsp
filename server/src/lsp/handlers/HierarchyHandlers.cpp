#include "lsp/Server.h"
#include "features/call_hierarchy/CallHierarchyHandler.h"
#include "features/type_hierarchy/TypeHierarchyHandler.h"
#include "lsp/PositionCodec.h"

namespace angel_lsp
{
    void Server::RegisterHierarchyHandlers()
    {
        m_messageHandler->add<lsp::requests::TextDocument_PrepareCallHierarchy>(
            [this](lsp::requests::TextDocument_PrepareCallHierarchy::Params &&req) -> lsp::requests::TextDocument_PrepareCallHierarchy::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }
                const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
                if (!doc)
                {
                    return lsp::Null{};
                }

                features::CallHierarchyPrepareRequest pr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                auto items = features::PrepareCallHierarchy(pr);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::CallHierarchy_IncomingCalls>(
            [this](lsp::requests::CallHierarchy_IncomingCalls::Params &&req) -> lsp::requests::CallHierarchy_IncomingCalls::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }

                features::CallHierarchyItemRequest ir{ m_symbolTable, m_callGraph, req.item };
                auto calls = features::GetIncomingCalls(ir);
                if (!calls.has_value() || calls->empty())
                {
                    return lsp::Null{};
                }
                for (auto &call : calls.value())
                {
                    EncodeItemRanges(call.from);
                    EncodeRangesIn(DocumentKey(call.from.uri.toString()), call.fromRanges);
                }
                return calls.value();
            });

        m_messageHandler->add<lsp::requests::CallHierarchy_OutgoingCalls>(
            [this](lsp::requests::CallHierarchy_OutgoingCalls::Params &&req) -> lsp::requests::CallHierarchy_OutgoingCalls::Result
            {
                if (!m_config.features.enableCallHierarchy)
                {
                    return lsp::Null{};
                }

                // fromRanges are ranges in the caller, which is the item the client asked about -
                // not in the callee the entry points at.
                const std::string callerUri = DocumentKey(req.item.uri.toString());

                features::CallHierarchyItemRequest ir{ m_symbolTable, m_callGraph, req.item };
                auto calls = features::GetOutgoingCalls(ir);
                if (!calls.has_value() || calls->empty())
                {
                    return lsp::Null{};
                }
                for (auto &call : calls.value())
                {
                    EncodeItemRanges(call.to);
                    EncodeRangesIn(callerUri, call.fromRanges);
                }
                return calls.value();
            });

        m_messageHandler->add<lsp::requests::TextDocument_PrepareTypeHierarchy>(
            [this](lsp::requests::TextDocument_PrepareTypeHierarchy::Params &&req) -> lsp::requests::TextDocument_PrepareTypeHierarchy::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }
                const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
                if (!doc)
                {
                    return lsp::Null{};
                }

                features::TypeHierarchyPrepareRequest pr{
                    doc->uri, *doc->text, doc->tree, m_symbolTable,
                    codec::Decode(*doc->text, m_positionEncoding, req.position)
                };
                auto items = features::PrepareTypeHierarchy(pr);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::TypeHierarchy_Supertypes>(
            [this](lsp::requests::TypeHierarchy_Supertypes::Params &&req) -> lsp::requests::TypeHierarchy_Supertypes::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }

                features::TypeHierarchyItemRequest ir{ m_symbolTable, req.item };
                auto items = features::GetSupertypes(ir);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });

        m_messageHandler->add<lsp::requests::TypeHierarchy_Subtypes>(
            [this](lsp::requests::TypeHierarchy_Subtypes::Params &&req) -> lsp::requests::TypeHierarchy_Subtypes::Result
            {
                if (!m_config.features.enableTypeHierarchy)
                {
                    return lsp::Null{};
                }

                features::TypeHierarchyItemRequest ir{ m_symbolTable, req.item };
                auto items = features::GetSubtypes(ir);
                if (!items.has_value() || items->empty())
                {
                    return lsp::Null{};
                }
                for (auto &item : items.value())
                {
                    EncodeItemRanges(item);
                }
                return items.value();
            });
    }
}
