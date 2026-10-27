#include "features/breakpoints/BreakpointLocationsHandler.h"
#include "lsp/PositionCodec.h"
#include "lsp/Server.h"
#include "utils/PositionEncoding.h"

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace angel_lsp
{
namespace
{

/**
 * @brief Parsed parameters for breakpoint locations requests.
 */
struct BreakpointLocationsParams
{
    std::string uri;
    uint32_t startLine = 0;
    uint32_t endLine = std::numeric_limits<uint32_t>::max();
};

/**
 * @brief Extracts the document URI from JSON-RPC parameters.
 * @param[in] obj Parameter object.
 * @return Extracted URI string or empty if missing.
 */
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

/**
 * @brief Parses line numbers safely from the optional range parameter.
 * @param[in] obj Parameter object.
 * @param[out] startLine Starting line number.
 * @param[out] endLine Ending line number.
 */
void ExtractRangeLines(const lsp::json::Object& obj, uint32_t& startLine, uint32_t& endLine)
{
    const auto* rangeVal = obj.find("range");
    if (!rangeVal || !rangeVal->isObject())
    {
        return;
    }

    const auto& rangeObj = rangeVal->object();
    if (const auto* startVal = rangeObj.find("start"); startVal && startVal->isObject())
    {
        if (const auto* lineVal = startVal->object().find("line"); lineVal && lineVal->isNumber())
        {
            const double num = lineVal->number();
            if (num >= 0.0)
            {
                startLine = static_cast<uint32_t>(num);
            }
        }
    }
    if (const auto* endVal = rangeObj.find("end"); endVal && endVal->isObject())
    {
        if (const auto* lineVal = endVal->object().find("line"); lineVal && lineVal->isNumber())
        {
            const double num = lineVal->number();
            if (num >= 0.0)
            {
                endLine = static_cast<uint32_t>(num);
            }
        }
    }
}

/**
 * @brief Parses incoming JSON-RPC breakpoint location request parameters.
 * @param[in] params Raw JSON value.
 * @return Populated parameter struct.
 */
BreakpointLocationsParams ParseBreakpointParams(const lsp::json::Value& params)
{
    BreakpointLocationsParams parsed;
    if (!params.isObject())
    {
        return parsed;
    }

    const auto& obj = params.object();
    parsed.uri = ExtractDocumentUri(obj);
    ExtractRangeLines(obj, parsed.startLine, parsed.endLine);
    return parsed;
}

/**
 * @brief Formats a single breakpoint location into an LSP JSON object.
 * @param[in] item Breakpoint location item with byte-based columns.
 * @param[in] text Document source text.
 * @param[in] lineIndex Precomputed line index for O(1) multi-line UTF-16 conversions.
 * @param[in] enc Client position encoding.
 * @return LSP-compliant BreakpointLocation JSON object.
 */
lsp::json::Object FormatBreakpointLocation(const features::BreakpointLocationItem& item, std::string_view text,
                                           const utils::LineIndex* lineIndex, utils::PositionEncoding enc)
{
    lsp::Range range;
    range.start.line = item.line;
    range.start.character = item.column;
    range.end.line = item.endLine;
    range.end.character = item.endColumn;

    if (lineIndex != nullptr)
    {
        codec::Encode(*lineIndex, text, enc, range);
    }
    else
    {
        codec::Encode(text, enc, range);
    }

    lsp::json::Object startObj;
    startObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.line));
    startObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.start.character));

    lsp::json::Object endObj;
    endObj["line"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.end.line));
    endObj["character"] = lsp::json::Value(static_cast<lsp::json::Integer>(range.end.character));

    lsp::json::Object rangeObj;
    rangeObj["start"] = lsp::json::Value(std::move(startObj));
    rangeObj["end"] = lsp::json::Value(std::move(endObj));

    lsp::json::Object locObj;
    locObj["range"] = lsp::json::Value(std::move(rangeObj));
    return locObj;
}

} // namespace

lsp::json::Value Server::HandleRequestsTextDocument_BreakpointLocations(lsp::json::Value&& params)
{
    const auto parsed = ParseBreakpointParams(params);
    if (parsed.uri.empty() || parsed.startLine > parsed.endLine)
    {
        return lsp::json::Value(lsp::json::Array{});
    }

    const auto doc = LookupOpenDocument(parsed.uri);
    if (!doc || !doc->tree || !doc->text)
    {
        return lsp::json::Value(lsp::json::Array{});
    }

    const auto excluded = ExcludedLineRanges(*doc->text);
    const features::BreakpointLocationsRequest req{ts_tree_root_node(doc->tree), parsed.startLine, parsed.endLine,
                                                   &excluded};

    const auto items = features::BreakpointLocationsHandler::Collect(req);
    lsp::json::Array result;
    result.reserve(items.size());

    std::optional<utils::LineIndex> lineIndex;
    if (m_positionEncoding != utils::PositionEncoding::Utf8 && !items.empty())
    {
        lineIndex = utils::LineIndex::Build(*doc->text);
    }
    const utils::LineIndex* pLineIndex = lineIndex.has_value() ? &*lineIndex : nullptr;

    for (const auto& item : items)
    {
        result.push_back(lsp::json::Value(FormatBreakpointLocation(item, *doc->text, pLineIndex, m_positionEncoding)));
    }

    return lsp::json::Value(std::move(result));
}

} // namespace angel_lsp