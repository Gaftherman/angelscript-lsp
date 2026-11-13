/**
 * @file CodeActionTypos.cpp
 * @brief Quick-fix code actions suggesting corrections for typo identifiers.
 */

#include "analysis/code_action/CodeActionInternal.h"
#include "utils/TransparentHash.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Collects all local definition names visible at the given scope chain.
 * @param[in] scope Starting lexical scope.
 * @param[out] names Destination names vector.
 */
static void CollectVisibleLocalNames(const analysis::Scope* scope, std::vector<std::string>& names)
{
    for (const analysis::Scope* walk = scope; walk != nullptr; walk = walk->parent)
    {
        for (const auto& def : walk->definitions)
        {
            names.push_back(def.name);
        }
    }
}

/**
 * @brief Tests if symbol type can appear where a type name is expected.
 * @param[in] type Symbol type.
 * @return True if symbol is class, interface, enum, typedef, or funcdef.
 */
static bool IsTypeLikeSymbol(analysis::SymbolType type)
{
    return type == analysis::SymbolType::Class || type == analysis::SymbolType::Interface ||
           type == analysis::SymbolType::Enum || type == analysis::SymbolType::Typedef ||
           type == analysis::SymbolType::Funcdef;
}

/**
 * @brief Collects candidate names for typo suggestions from local scopes and symbols.
 * @param[in] query Incoming code action query context.
 * @param[in] line Cursor line.
 * @param[in] isIdentifier True if unresolved target is an identifier.
 * @param[in] isType True if unresolved target is a type.
 * @return Vector of candidate names.
 */
static std::vector<std::string> CollectTypoCandidates(const CodeActionQuery& query, uint32_t line, bool isIdentifier,
                                                      bool isType)
{
    std::vector<std::string> candidates;
    if (isIdentifier)
    {
        auto rootScope = query.scopeIndex.GetRoot(query.uri);
        if (rootScope)
        {
            CollectVisibleLocalNames(FindScopeByLineOrRoot(rootScope.get(), line), candidates);
        }
    }
    query.symbolTable.ForEachSymbol(
        [&candidates, isType](std::string_view name, const std::vector<analysis::Symbol>& symbols)
        {
            if (isType && std::none_of(symbols.begin(), symbols.end(),
                                       [](const analysis::Symbol& sym) { return IsTypeLikeSymbol(sym.type); }))
            {
                return;
            }
            candidates.emplace_back(name);
        });
    return candidates;
}

/**
 * @brief Ranks typo candidate suggestions by edit distance.
 * @param[in] typed Typed identifier text.
 * @param[in] candidates Available candidate names.
 * @return Sorted vector of suggestions within distance limit.
 */
static std::vector<TypoCandidate> RankTypoSuggestions(std::string_view typed,
                                                      const std::vector<std::string>& candidates)
{
    const std::string typedFolded = FoldCase(typed);
    const size_t limit = SuggestionLimit(typed.size());
    std::vector<TypoCandidate> ranked;
    TransparentStringSet seen;

    for (const auto& candidate : candidates)
    {
        if (candidate.empty() || candidate == typed || !seen.insert(candidate).second)
        {
            continue;
        }
        const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidate), limit);
        if (distance <= limit)
        {
            ranked.push_back({candidate, distance});
        }
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const TypoCandidate& a, const TypoCandidate& b)
              {
                  if (a.distance != b.distance)
                  {
                      return a.distance < b.distance;
                  }
                  return a.name < b.name;
              });
    return ranked;
}

/**
 * @brief Emits quick-fix code action entries for typo suggestions.
 * @param[in] ctx Typo fix context.
 * @param[in] ranked Ranked suggestions list.
 * @param[out] entries Destination code action entries vector.
 */
static void EmitTypoEntries(const TypoFixContext& ctx, const std::vector<TypoCandidate>& ranked,
                            std::vector<CodeActionEntry>& entries)
{
    const size_t offered = std::min<size_t>(ranked.size(), 3);
    const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

    for (size_t i = 0; i < offered; ++i)
    {
        CodeActionEntry entry;
        entry.title = i18n::FormatMessage(ctx.query.i18n, "action-did-you-mean", "Did you mean '{}'?",
                                          std::string_view(ranked[i].name));
        entry.kind = CodeActionEntryKind::QuickFix;
        entry.diagnosticIndices = std::vector<uint32_t>{ctx.diagIndex};

        if (i == 0 && hasClearWinner)
        {
            entry.isPreferred = true;
        }

        entry.edits = std::vector<CodeActionTextEdit>{CodeActionTextEdit{ctx.identifierRange, ranked[i].name}};

        entries.push_back(std::move(entry));
    }
}

/**
 * @brief Generates quick-fix entries suggesting corrections for undefined identifiers and unknown types.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddUndefinedIdentifierSuggestions(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (!query.nodeIndex || query.sourceCode.empty())
    {
        return;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        const bool isIdentifier = MatchDiagnosticCode(diag, diagnostics::codes::UndefinedIdentifier);
        const bool isType = MatchDiagnosticCode(diag, diagnostics::codes::UnknownType);
        if (!isIdentifier && !isType)
        {
            continue;
        }

        const auto identifier =
            analysis::FindNodeSpanAtPoint(*query.nodeIndex, diag.range.startLine, diag.range.startCharacter);
        if (!identifier)
        {
            continue;
        }

        const std::string_view typed =
            utils::SliceSource(query.sourceCode, identifier->bytes.startByte, identifier->bytes.endByte);
        if (typed.empty())
        {
            continue;
        }

        auto candidates = CollectTypoCandidates(query, diag.range.startLine, isIdentifier, isType);
        auto ranked = RankTypoSuggestions(typed, candidates);
        if (!ranked.empty())
        {
            EmitTypoEntries(TypoFixContext{query, i, identifier->range}, ranked, entries);
        }
    }
}

} // namespace angel_lsp::analysis::code_action
