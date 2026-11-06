#include "features/workspace_symbol/WorkspaceSymbolHandler.h"

#include "features/common/FeatureSharedUtils.h"
#include "utils/Utils.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Computes the ranking score of a symbol against the workspace query, ignoring ASCII case.
 * @param[in] query Query text as typed by the user; exact-case hits rank above case-folded ones.
 * @param[in] sym Candidate symbol.
 * @return MatchResult indicating if matched and the ranking score (exact > prefix > substring > qualified > fuzzy).
 */
static MatchResult MatchSymbol(std::string_view query, const analysis::Symbol& sym)
{
    if (query.empty())
    {
        return {true, 1};
    }

    const std::string_view name = sym.name;

    if (utils::CaseInsensitiveEquals(name, query))
    {
        return {true, (name == query) ? 110 : 100};
    }

    if (utils::CaseInsensitiveStartsWith(name, query))
    {
        return {true, name.starts_with(query) ? 85 : 80};
    }

    if (utils::CaseInsensitiveContains(name, query))
    {
        return {true, 50};
    }

    if (utils::CaseInsensitiveContains(sym.qualifiedName, query))
    {
        return {true, 40};
    }

    // Fuzzy subsequence match on name
    size_t qIdx = 0;
    for (size_t i = 0; i < name.size() && qIdx < query.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(query[qIdx])))
        {
            qIdx++;
        }
    }
    if (qIdx == query.size())
    {
        return {true, 20};
    }

    return {false, 0};
}

std::optional<WorkspaceSymbolResult> GetWorkspaceSymbols(const WorkspaceSymbolRequest& request)
{
    std::vector<ScoredSymbol> matches;
    ankerl::unordered_dense::set<std::pair<std::string_view, uint64_t>> seenKeys;

    request.symbolTable.ForEachSymbol(
        [&]([[maybe_unused]] std::string_view qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (sym.isSynthesized || sym.type == analysis::SymbolType::CallReference)
                {
                    continue;
                }

                auto match = MatchSymbol(request.query, sym);
                if (match.matched)
                {
                    // Deduplicate identical declarations (e.g. enum dual indexing)
                    const uint64_t nameHash = ankerl::unordered_dense::hash<std::string_view>{}(sym.name);
                    const uint64_t coordKey =
                        (static_cast<uint64_t>(sym.startLine) << 32) | static_cast<uint64_t>(sym.startCharacter);
                    const uint64_t key =
                        coordKey ^ (nameHash + 0x9e3779b97f4a7c15ULL + (coordKey << 6) + (coordKey >> 2));
                    if (!seenKeys.insert({sym.fileUri, key}).second)
                    {
                        continue;
                    }

                    lsp::SymbolInformation info;
                    info.name = sym.name;
                    info.kind = common::ToLspSymbolKind(sym.type, !sym.containerName.empty());

                    if (!sym.containerName.empty())
                    {
                        info.containerName = sym.containerName;
                    }

                    info.location = lsp::Location{lsp::DocumentUri::parse(sym.fileUri),
                                                  lsp::Range{lsp::Position{sym.startLine, sym.startCharacter},
                                                             lsp::Position{sym.endLine, sym.endCharacter}}};

                    matches.push_back(ScoredSymbol{match.score, std::move(info)});
                }
            }
        });

    std::sort(matches.begin(), matches.end(),
              [](const ScoredSymbol& a, const ScoredSymbol& b)
              {
                  if (a.score != b.score)
                  {
                      return a.score > b.score;
                  }
                  return a.info.name < b.info.name;
              });

    WorkspaceSymbolResult result;
    size_t count = std::min(matches.size(), request.maxResults);
    result.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        result.push_back(std::move(matches[i].info));
    }

    return result;
}
} // namespace angel_lsp::features
