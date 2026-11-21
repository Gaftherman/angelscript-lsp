#include "analysis/index/LocalScopeCollector.h"
#include "analysis/index/NodeIndex.h"
#include "features/semantic_tokens/SemanticTokensHandler.h"
#include "i18n/i18n.h"
#include "lsp/Server.h"
#include "lsp/state/PositionCodec.h"
#include "utils/Timer.h"
#include "utils/Utils.h"

#include <vector>

namespace angel_lsp
{
static bool AreSemanticTokensEditsValid(const std::vector<lsp::SemanticTokensEdit>& edits)
{
    for (const auto& edit : edits)
    {
        if (edit.start % 5 != 0 || edit.deleteCount % 5 != 0 || (edit.data.has_value() && edit.data->size() % 5 != 0))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Builds a full wire result from an immutable snapshot.
 * @param[in] snapshot Encoded data retained by the request.
 * @return Full token payload with a result ID if it may be used as a delta base.
 */
static lsp::SemanticTokens FullTokenResponse(const SemanticTokensSnapshot& snapshot)
{
    lsp::SemanticTokens result;
    if (!snapshot.resultId.empty())
    {
        result.resultId = snapshot.resultId;
    }
    result.data = *snapshot.data;
    return result;
}

lsp::requests::TextDocument_SemanticTokens_Full::Result
Server::HandleRequestsTextDocument_SemanticTokens_Full(lsp::requests::TextDocument_SemanticTokens_Full::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    utils::HighResTimer timer;
    auto tokens = FullTokenResponse(ResolveSemanticTokensSnapshot(*doc));
    const double elapsedMs = timer.ElapsedMs();
    LogInfo(i18n::FormatMessage(m_i18n.get(), "log-semantic-tokens-full",
                                "[Semantic Tokens] Finished full in {:.2f} ms for {} ({} tokens)", elapsedMs, doc->uri,
                                tokens.data.size() / 5));
    return tokens;
}

lsp::requests::TextDocument_SemanticTokens_Full_Delta::Result
Server::HandleRequestsTextDocument_SemanticTokens_Full_Delta(
    lsp::requests::TextDocument_SemanticTokens_Full_Delta::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    utils::HighResTimer timer;
    const auto previous = FindSemanticTokensSnapshot(doc->uri);
    const auto current = ResolveSemanticTokensSnapshot(*doc);
    if (!previous || previous->resultId != req.previousResultId || previous->hasError || current.hasError ||
        current.resultId.empty())
    {
        return FullTokenResponse(current);
    }

    lsp::SemanticTokensDelta delta;
    delta.resultId = current.resultId;
    if (previous->resultId != current.resultId)
    {
        delta.edits = features::ComputeSemanticTokensDelta(*previous->data, *current.data);
    }
    if (!AreSemanticTokensEditsValid(delta.edits))
    {
        return FullTokenResponse(current);
    }
    LogInfo(i18n::FormatMessage(m_i18n.get(), "log-semantic-tokens-delta",
                                "[Semantic Tokens] Finished delta in {:.2f} ms for {} ({} edits)", timer.ElapsedMs(),
                                doc->uri, delta.edits.size()));
    return delta;
}

lsp::requests::TextDocument_SemanticTokens_Range::Result
Server::HandleRequestsTextDocument_SemanticTokens_Range(lsp::requests::TextDocument_SemanticTokens_Range::Params&& req)
{
    if (!m_config.features.enableSemanticTokens)
    {
        return lsp::Null{};
    }
    const auto doc = LookupOpenDocument(req.textDocument.uri.toString());
    if (!doc)
    {
        return lsp::Null{};
    }

    const auto syntax = ResolveProtocolSyntax(*doc).syntax;
    const lsp::Range decodedRange = codec::Decode(*doc->text, GetPositionEncoding(), req.range);
    const analysis::SourceRange factsRange{decodedRange.start.line, decodedRange.start.character, decodedRange.end.line,
                                           decodedRange.end.character};
    const analysis::SemanticTokenFacts facts =
        syntax ? analysis::CollectSemanticTokenFacts(syntax->index, *doc->text, factsRange)
               : analysis::SemanticTokenFacts{};
    features::SemanticTokensRequest sr{doc->uri, *doc->text, facts, m_symbolTable};
    sr.scopeRoot = m_scopeIndex.GetRoot(doc->uri);
    if (syntax && m_localScopeCollector && !sr.scopeRoot)
    {
        sr.scopeRoot = m_localScopeCollector->CollectScopesFromIndex(syntax->index, *doc->text);
    }
    sr.excludedLineRanges = ExcludedLineRanges(*doc->text);
    sr.range = decodedRange;

    lsp::SemanticTokens tokens = features::GetSemanticTokens(sr);
    codec::EncodeSemanticTokens(*doc->text, GetPositionEncoding(), tokens.data);
    return tokens;
}
} // namespace angel_lsp
