#include "features/breakpoints/EvaluatableExpressionHandler.h"

#include "analysis/index/EvaluatableFacts.h"

#include <optional>
#include <string_view>

namespace angel_lsp::features
{

std::optional<EvaluatableExpressionItem>
EvaluatableExpressionHandler::Extract(const EvaluatableExpressionRequest& request)
{
    if (!request.nodeIndex || request.sourceCode.empty())
    {
        return std::nullopt;
    }

    auto span = analysis::FindEvaluatableSpan(*request.nodeIndex, request.line, request.column);
    if (!span)
    {
        return std::nullopt;
    }

    EvaluatableExpressionItem item;
    item.line = span->range.startLine;
    item.column = span->range.startCharacter;
    item.endLine = span->range.endLine;
    item.endColumn = span->range.endCharacter;
    if (span->startByte < span->endByte && span->endByte <= request.sourceCode.size())
    {
        item.expression = request.sourceCode.substr(span->startByte, span->endByte - span->startByte);
    }
    return item;
}

} // namespace angel_lsp::features
