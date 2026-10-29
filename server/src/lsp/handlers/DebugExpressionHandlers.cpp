#include "features/breakpoints/EvaluatableExpressionHandler.h"
#include "features/completion/CompletionHandler.h"
#include "lsp/PositionCodec.h"
#include "lsp/Server.h"
#include "utils/PositionEncoding.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{

namespace
{

std::string ExtractDocumentUri(const lsp::json::Object& obj)
{
    if (const auto* docVal = obj.find("textDocument"); docVal && docVal->isObject())
    {
        if (const auto* uriVal = docVal->object().find("uri"); uriVal && uriVal->isString())
        {
            return uriVal->string();
        }
    }
    return {};
}

lsp::Position ExtractPosition(const lsp::json::Object& obj)
{
    lsp::Position pos{0, 0};
    if (const auto* posVal = obj.find("position"); posVal && posVal->isObject())
    {
        const auto& posObj = posVal->object();
        if (const auto* lVal = posObj.find("line"); lVal && lVal->isNumber())
        {
            pos.line = static_cast<uint32_t>(lVal->number());
        }
        if (const auto* cVal = posObj.find("character"); cVal && cVal->isNumber())
        {
            pos.character = static_cast<uint32_t>(cVal->number());
        }
    }
    return pos;
}

lsp::json::Object FormatEvaluatableResponse(const features::EvaluatableExpressionItem& item, std::string_view text,
                                            utils::PositionEncoding enc)
{
    lsp::Range outRange;
    outRange.start.line = item.line;
    outRange.start.character = item.column;
    outRange.end.line = item.endLine;
    outRange.end.character = item.endColumn;
    codec::Encode(text, enc, outRange);

    lsp::json::Object startObj;
    startObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(outRange.start.line));
    startObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(outRange.start.character));

    lsp::json::Object endObj;
    endObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(outRange.end.line));
    endObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(outRange.end.character));

    lsp::json::Object rangeObj;
    rangeObj["start"] = lsp::json::Value(std::move(startObj));
    rangeObj["end"] = lsp::json::Value(std::move(endObj));

    lsp::json::Object resp;
    resp["range"] = lsp::json::Value(std::move(rangeObj));
    if (!item.expression.empty())
    {
        resp["expression"] = lsp::json::Value(std::string(item.expression));
    }
    return resp;
}

} // namespace

lsp::json::Value Server::HandleRequestsTextDocument_EvaluatableExpression(lsp::json::Value&& params)
{
    if (!params.isObject())
    {
        return lsp::json::Value(nullptr);
    }

    const auto& obj = params.object();
    const std::string uri = ExtractDocumentUri(obj);
    if (uri.empty())
    {
        return lsp::json::Value(nullptr);
    }

    const auto doc = LookupOpenDocument(uri);
    if (!doc || !doc->tree || !doc->text)
    {
        return lsp::json::Value(nullptr);
    }

    const lsp::Position rawPos = ExtractPosition(obj);
    const lsp::Position decodedPos = codec::Decode(*doc->text, m_positionEncoding, rawPos);

    const features::EvaluatableExpressionRequest req{ts_tree_root_node(doc->tree), *doc->text, decodedPos.line,
                                                     decodedPos.character};

    const auto result = features::EvaluatableExpressionHandler::Extract(req);
    if (!result)
    {
        return lsp::json::Value(nullptr);
    }

    return lsp::json::Value(FormatEvaluatableResponse(*result, *doc->text, m_positionEncoding));
}

lsp::json::Value Server::HandleRequestsAngelscript_DebugCompletion(lsp::json::Value&& params)
{
    if (!params.isObject())
    {
        return lsp::json::Value(lsp::json::Array{});
    }

    const auto& obj = params.object();
    const std::string uri = ExtractDocumentUri(obj);
    if (uri.empty())
    {
        return lsp::json::Value(lsp::json::Array{});
    }

    const auto doc = LookupOpenDocument(uri);
    if (!doc || !doc->tree || !doc->text)
    {
        return lsp::json::Value(lsp::json::Array{});
    }

    uint32_t line = 0;
    if (const auto* lVal = obj.find("line"); lVal && lVal->isNumber())
    {
        line = static_cast<uint32_t>(lVal->number());
    }

    features::CompletionRequest cr{doc->uri,      *doc->text,      doc->tree,
                                   m_symbolTable, m_scopeIndex,    lsp::Position{line, 0},
                                   &m_config,     m_snippetSupport};
    cr.documentPath = CanonicalPathFromUri(doc->uri);
    cr.implicitExtension = std::string(ImplicitIncludeExtension());
    cr.listIncludeCandidates = [this]() { return IncludableFiles(); };

    const auto items = features::GetCompletion(cr);
    lsp::json::Array arr;
    arr.reserve(items.size());
    for (const auto& item : items)
    {
        lsp::json::Object itemObj;
        itemObj["label"] = lsp::json::Value(std::string(item.label));
        if (item.kind.has_value())
        {
            itemObj["kind"] = lsp::json::Value(static_cast<lsp::json::Integer>(*item.kind));
        }
        if (item.detail.has_value())
        {
            itemObj["detail"] = lsp::json::Value(std::string(*item.detail));
        }
        arr.push_back(lsp::json::Value(std::move(itemObj)));
    }
    return lsp::json::Value(std::move(arr));
}

} // namespace angel_lsp
