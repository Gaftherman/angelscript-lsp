#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Collects all local definition names visible at the given scope chain.
 * @param[in] scope Starting lexical scope.
 * @param[out] names Destination names vector.
 */
void CollectVisibleLocalNames(const analysis::Scope* scope, std::vector<std::string>& names)
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
bool IsTypeLikeSymbol(analysis::SymbolType type)
{
    return type == analysis::SymbolType::Class || type == analysis::SymbolType::Interface ||
           type == analysis::SymbolType::Enum || type == analysis::SymbolType::Typedef ||
           type == analysis::SymbolType::Funcdef;
}

struct TypoCandidateSuggestion
{
    std::string name;
    size_t distance = 0;
};

/**
 * @brief Collects candidate names for typo suggestions from local scopes and symbols.
 * @param[in] request Code action request.
 * @param[in] point Cursor position.
 * @param[in] isIdentifier True if unresolved target is an identifier.
 * @param[in] isType True if unresolved target is a type.
 * @return Vector of candidate names.
 */
std::vector<std::string> CollectTypoCandidates(const CodeActionRequest& request, TSPoint point, bool isIdentifier,
                                               bool isType)
{
    std::vector<std::string> candidates;
    if (isIdentifier)
    {
        auto rootScope = request.scopeIndex.GetRoot(request.uri);
        if (rootScope)
        {
            CollectVisibleLocalNames(FindScopeByLineOrRoot(rootScope.get(), point.row), candidates);
        }
    }
    request.symbolTable.ForEachSymbol(
        [&candidates, isType](const std::string& name, const std::vector<analysis::Symbol>& symbols)
        {
            if (isType && std::none_of(symbols.begin(), symbols.end(),
                                       [](const analysis::Symbol& sym) { return IsTypeLikeSymbol(sym.type); }))
            {
                return;
            }
            candidates.push_back(name);
        });
    return candidates;
}

/**
 * @brief Ranks typo candidate suggestions by edit distance.
 * @param[in] typed Typed identifier text.
 * @param[in] candidates Available candidate names.
 * @return Sorted vector of suggestions within distance limit.
 */
std::vector<TypoCandidateSuggestion> RankTypoSuggestions(const std::string& typed,
                                                         const std::vector<std::string>& candidates)
{
    const std::string typedFolded = FoldCase(typed);
    const size_t limit = SuggestionLimit(typed.size());
    std::vector<TypoCandidateSuggestion> ranked;
    ankerl::unordered_dense::set<std::string> seen;

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
              [](const TypoCandidateSuggestion& a, const TypoCandidateSuggestion& b)
              {
                  if (a.distance != b.distance)
                  {
                      return a.distance < b.distance;
                  }
                  return a.name < b.name;
              });
    return ranked;
}

struct TypoFixContext
{
    const CodeActionRequest& request;
    const lsp::Diagnostic& diag;
    TSNode identifier;
};

/**
 * @brief Emits quick-fix code actions for typo suggestions.
 * @param[in] ctx Typo fix context.
 * @param[in] ranked Ranked suggestions list.
 * @param[out] actions Destination actions vector.
 */
void EmitTypoCodeActions(const TypoFixContext& ctx, const std::vector<TypoCandidateSuggestion>& ranked,
                         std::vector<lsp::CodeAction>& actions)
{
    const size_t offered = std::min<size_t>(ranked.size(), 3);
    const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

    for (size_t i = 0; i < offered; ++i)
    {
        lsp::CodeAction action;
        action.title = "Did you mean '" + ranked[i].name + "'?";
        action.kind = lsp::CodeActionKind::QuickFix;
        action.diagnostics = std::vector<lsp::Diagnostic>{ctx.diag};

        if (i == 0 && hasClearWinner)
        {
            action.isPreferred = true;
        }

        lsp::TextEdit edit;
        edit.range.start.line = ts_node_start_point(ctx.identifier).row;
        edit.range.start.character = ts_node_start_point(ctx.identifier).column;
        edit.range.end.line = ts_node_end_point(ctx.identifier).row;
        edit.range.end.character = ts_node_end_point(ctx.identifier).column;
        edit.newText = ranked[i].name;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(ctx.request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

} // namespace

void TryAddUndefinedIdentifierSuggestions(const CodeActionRequest& request, TSNode rootNode,
                                          std::vector<lsp::CodeAction>& actions)
{
    if (ts_node_is_null(rootNode) || request.sourceCode.empty())
    {
        return;
    }

    for (const auto& diag : request.context.diagnostics)
    {
        const bool isIdentifier = MatchDiagnosticCode(diag, diagnostics::codes::UndefinedIdentifier);
        const bool isType = MatchDiagnosticCode(diag, diagnostics::codes::UnknownType);
        if (!isIdentifier && !isType)
        {
            continue;
        }

        const TSPoint point = {diag.range.start.line, diag.range.start.character};
        TSNode identifier = ts_node_descendant_for_point_range(rootNode, point, point);
        if (ts_node_is_null(identifier))
        {
            continue;
        }

        const std::string typed = GetNodeText(identifier, request.sourceCode);
        if (typed.empty())
        {
            continue;
        }

        auto candidates = CollectTypoCandidates(request, point, isIdentifier, isType);
        auto ranked = RankTypoSuggestions(typed, candidates);
        if (!ranked.empty())
        {
            EmitTypoCodeActions(TypoFixContext{request, diag, identifier}, ranked, actions);
        }
    }
}

} // namespace angel_lsp::features
