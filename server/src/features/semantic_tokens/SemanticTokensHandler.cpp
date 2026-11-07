#include "features/semantic_tokens/SemanticTokensHandler.h"

#include "features/semantic_tokens/SemanticTokensInternal.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Encodes sorted, non-overlapping semantic tokens into LSP 5-tuple integer stream.
 * @param[in] tokens Tokens to encode.
 * @return SemanticTokens struct containing delta-encoded data stream.
 */
static lsp::SemanticTokens DeltaEncodeTokens(const std::vector<RawToken>& tokens)
{
    std::vector<lsp::uint> data;
    data.reserve(tokens.size() * 5);

    uint32_t prevLine = 0;
    uint32_t prevChar = 0;

    for (const auto& tok : tokens)
    {
        uint32_t deltaLine = tok.line - prevLine;
        uint32_t deltaChar = (deltaLine == 0) ? (tok.startChar - prevChar) : tok.startChar;

        data.push_back(deltaLine);
        data.push_back(deltaChar);
        data.push_back(tok.length);
        data.push_back(tok.tokenType);
        data.push_back(tok.tokenModifiers);

        prevLine = tok.line;
        prevChar = tok.startChar;
    }

    return lsp::SemanticTokens{std::move(data)};
}

const lsp::SemanticTokensLegend& GetSemanticTokensLegend()
{
    static const lsp::SemanticTokensLegend legend = {
        /* tokenTypes */ {"namespace",     "type",      "class",    "enum",     "interface",  "struct",
                          "typeParameter", "parameter", "variable", "property", "enumMember", "event",
                          "function",      "method",    "macro",    "keyword",  "modifier",   "comment",
                          "string",        "number",    "regexp",   "operator", "decorator",  "templatePunctuation"},
        /* tokenModifiers */ {"declaration", "definition", "readonly", "static", "deprecated", "abstract", "async",
                              "modification", "documentation", "defaultLibrary"}};
    return legend;
}

lsp::SemanticTokens GetSemanticTokens(const SemanticTokensRequest& request)
{
    if (request.sourceCode.empty() || request.facts.captures.empty())
    {
        return lsp::SemanticTokens{};
    }

    auto scopedEnumUpgrades = PrecalculateScopedEnumUpgrades(request.facts, request.sourceCode, request.symbolTable);

    ankerl::unordered_dense::map<uint64_t, analysis::LocalDefinitionKind> referenceKinds;
    if (request.scopeRoot)
    {
        CollectReferenceKinds(request.scopeRoot.get(), referenceKinds);
    }

    TokenRefinementContext ctx{request, referenceKinds, scopedEnumUpgrades, request.symbolTable.GetRuleIndex()};

    std::vector<RawToken> rawTokens;
    auto sourceLines = SplitLinesView(request.sourceCode);
    CollectHighlightsTokens(ctx, sourceLines, rawTokens);

    if (!request.excludedLineRanges.empty())
    {
        std::erase_if(rawTokens, [&request](const RawToken& tok)
                      { return angel_lsp::utils::IsLineExcluded(request.excludedLineRanges, tok.line); });
    }

    if (rawTokens.empty())
    {
        return lsp::SemanticTokens{};
    }

    std::vector<RawToken> filteredTokens = DeduplicateTokens(rawTokens);
    if (request.range.has_value())
    {
        NarrowTokensToRange(filteredTokens, *request.range);
    }
    FilterInvalidOperators(filteredTokens, sourceLines);

    return DeltaEncodeTokens(filteredTokens);
}

std::vector<lsp::SemanticTokensEdit> ComputeSemanticTokensDelta(const std::vector<lsp::uint>& previous,
                                                                const std::vector<lsp::uint>& current)
{
    if (previous == current)
    {
        return {};
    }

    if (previous.size() % 5 != 0 || current.size() % 5 != 0)
    {
        lsp::SemanticTokensEdit edit;
        edit.start = 0;
        edit.deleteCount = static_cast<lsp::uint>(previous.size());
        if (!current.empty())
        {
            edit.data = lsp::Array<lsp::uint>(current.begin(), current.end());
        }
        return {std::move(edit)};
    }

    size_t prefix = 0;
    const size_t shortest = std::min(previous.size(), current.size());
    while (prefix < shortest && previous[prefix] == current[prefix])
    {
        ++prefix;
    }
    prefix = prefix - (prefix % 5);

    size_t suffix = 0;
    while (suffix < shortest - prefix && previous[previous.size() - 1 - suffix] == current[current.size() - 1 - suffix])
    {
        ++suffix;
    }
    suffix = suffix - (suffix % 5);

    lsp::SemanticTokensEdit edit;
    edit.start = static_cast<lsp::uint>(prefix);
    edit.deleteCount = static_cast<lsp::uint>(previous.size() - prefix - suffix);

    if (current.size() - prefix - suffix > 0)
    {
        edit.data = lsp::Array<lsp::uint>(current.begin() + static_cast<std::ptrdiff_t>(prefix),
                                          current.end() - static_cast<std::ptrdiff_t>(suffix));
    }

    return {std::move(edit)};
}

} // namespace angel_lsp::features
