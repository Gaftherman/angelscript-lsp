#include "features/code_action/CodeActionInternal.h"

namespace angel_lsp::features
{
namespace
{

/**
 * @brief Collects all indexed document URIs known to the symbol table.
 * @param[in] table Symbol table.
 * @return Set of known document URIs.
 */
ankerl::unordered_dense::set<std::string> CollectIndexedFileUris(const analysis::SymbolTable& table)
{
    ankerl::unordered_dense::set<std::string> indexedUris;
    table.ForEachSymbol(
        [&indexedUris]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symbols)
        {
            for (const auto& sym : symbols)
            {
                if (!sym.fileUri.empty())
                {
                    indexedUris.insert(sym.fileUri);
                }
            }
        });
    return indexedUris;
}

struct IncludeCandidate
{
    std::string spelling;
    size_t distance = 0;
};

/**
 * @brief Searches indexed document URIs for filenames similar to the unresolved include.
 * @param[in] includerPath Absolute filesystem path of current document.
 * @param[in] rawPath Unresolved include directive raw path.
 * @param[in] indexedUris Set of indexed file URIs.
 * @param[in] currentUri Current document URI.
 * @return Ranked candidates list sorted by edit distance.
 */
std::vector<IncludeCandidate> RankIncludeCandidates(const std::string& includerPath, const std::string& rawPath,
                                                    const ankerl::unordered_dense::set<std::string>& indexedUris,
                                                    const std::string& currentUri)
{
    const std::string typedName = std::filesystem::path(rawPath).filename().generic_string();
    if (typedName.empty())
    {
        return {};
    }

    const std::string typedFolded = FoldCase(typedName);
    const size_t limit = SuggestionLimit(typedName.size());
    std::vector<IncludeCandidate> ranked;
    ankerl::unordered_dense::set<std::string> seen;

    for (const std::string& candidateUri : indexedUris)
    {
        if (candidateUri == currentUri)
        {
            continue;
        }

        const std::string candidatePath = angel_lsp::utils::UriToPath(candidateUri);
        if (candidatePath.empty())
        {
            continue;
        }

        const std::string candidateName = std::filesystem::path(candidatePath).filename().generic_string();
        const size_t distance = BoundedEditDistance(typedFolded, FoldCase(candidateName), limit);
        if (distance > limit)
        {
            continue;
        }

        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            std::filesystem::path(candidatePath), std::filesystem::path(includerPath).parent_path(), relativeError);
        if (relativeError || relative.empty())
        {
            continue;
        }

        const std::string spelling = relative.generic_string();
        if (spelling == rawPath || !seen.insert(spelling).second)
        {
            continue;
        }

        ranked.push_back({spelling, distance});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const IncludeCandidate& a, const IncludeCandidate& b)
              {
                  if (a.distance != b.distance)
                  {
                      return a.distance < b.distance;
                  }
                  return a.spelling < b.spelling;
              });
    return ranked;
}

struct IncludeFixContext
{
    const CodeActionRequest& request;
    const lsp::Diagnostic& diag;
    const angel_lsp::utils::IncludeDirective& directive;
};

/**
 * @brief Emits quick-fix code actions for unresolved include suggestions.
 * @param[in] ctx Include fix context.
 * @param[in] ranked Ranked include candidates.
 * @param[out] actions Destination actions vector.
 */
void EmitIncludeSuggestions(const IncludeFixContext& ctx, const std::vector<IncludeCandidate>& ranked,
                            std::vector<lsp::CodeAction>& actions)
{
    const std::string_view line =
        angel_lsp::utils::GetLine(ctx.request.sourceCode, static_cast<uint32_t>(ctx.directive.line));
    const char open = ctx.directive.isAngled ? '<' : '"';
    const char close = ctx.directive.isAngled ? '>' : '"';
    const size_t openPos = line.find(open);
    if (openPos == std::string_view::npos)
    {
        return;
    }
    const size_t closePos = line.find(close, openPos + 1);
    if (closePos == std::string_view::npos)
    {
        return;
    }

    const size_t offered = std::min<size_t>(ranked.size(), 3);
    const bool hasClearWinner = ranked.size() == 1 || ranked[0].distance < ranked[1].distance;

    for (size_t i = 0; i < offered; ++i)
    {
        lsp::CodeAction action;
        action.title = "Did you mean '" + ranked[i].spelling + "'?";
        action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::QuickFix);
        action.diagnostics = std::vector<lsp::Diagnostic>{ctx.diag};
        if (i == 0 && hasClearWinner)
        {
            action.isPreferred = true;
        }

        lsp::TextEdit edit;
        edit.range.start.line = static_cast<uint32_t>(ctx.directive.line);
        edit.range.start.character = static_cast<uint32_t>(openPos + 1);
        edit.range.end.line = static_cast<uint32_t>(ctx.directive.line);
        edit.range.end.character = static_cast<uint32_t>(closePos);
        edit.newText = ranked[i].spelling;

        lsp::WorkspaceEdit wsEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
        changes[lsp::DocumentUri::parse(ctx.request.uri)] = {std::move(edit)};
        wsEdit.changes = std::move(changes);
        action.edit = std::move(wsEdit);

        actions.push_back(std::move(action));
    }
}

struct IncludeRefCheckContext
{
    const std::string& currentUri;
    const analysis::SymbolTable& table;
    const ankerl::unordered_dense::set<std::string>& docReferences;
    const std::vector<std::string>& allowedRoots;
};

/**
 * @brief Checks whether an include directive is referenced by symbols used in the file.
 * @param[in] inc Include directive.
 * @param[in] ctx Include reference checking context.
 * @return True if include is referenced or has unknown symbols.
 */
bool IsIncludeDirectiveReferenced(const angel_lsp::utils::IncludeDirective& inc, const IncludeRefCheckContext& ctx)
{
    std::string resolved =
        utils::IncludeResolver::ResolveIncludePath(inc.rawPath, ctx.currentUri, {}, ctx.allowedRoots);
    std::vector<std::string> symbolsInFile;
    ctx.table.ForEachSymbol(
        [&]([[maybe_unused]] const std::string& qualifiedName, const std::vector<analysis::Symbol>& symList)
        {
            for (const auto& s : symList)
            {
                if ((!resolved.empty() && s.fileUri == resolved) ||
                    (!inc.rawPath.empty() && s.fileUri.find(inc.rawPath) != std::string::npos))
                {
                    symbolsInFile.push_back(s.name);
                }
            }
        });

    if (symbolsInFile.empty())
    {
        return true;
    }
    for (const auto& symName : symbolsInFile)
    {
        if (ctx.docReferences.contains(symName))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Formats sorted angled and quoted include directive blocks.
 * @param[in] angledIncludes Sorted vector of angled include paths.
 * @param[in] quotedIncludes Sorted vector of quoted include paths.
 * @return Formatted include header block text.
 */
std::string FormatSortedIncludesBlock(const std::vector<std::string>& angledIncludes,
                                      const std::vector<std::string>& quotedIncludes)
{
    std::string newHeaderBlock;
    for (const auto& p : angledIncludes)
    {
        newHeaderBlock += "#include <" + p + ">\n";
    }
    if (!angledIncludes.empty() && !quotedIncludes.empty())
    {
        newHeaderBlock += "\n";
    }
    for (const auto& p : quotedIncludes)
    {
        newHeaderBlock += "#include \"" + p + "\"\n";
    }
    return newHeaderBlock;
}

} // namespace

void TryAddUnresolvedIncludeSuggestions(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    if (request.sourceCode.empty())
    {
        return;
    }
    const bool anyUnresolvedInclude =
        std::any_of(request.context.diagnostics.begin(), request.context.diagnostics.end(),
                    [](const lsp::Diagnostic& diag) { return MatchDiagnosticCode(diag, "as-warn-include-not-found"); });
    if (!anyUnresolvedInclude)
    {
        return;
    }
    const std::string includerPath = angel_lsp::utils::UriToPath(request.uri);
    if (includerPath.empty())
    {
        return;
    }

    auto indexedUris = CollectIndexedFileUris(request.symbolTable);
    const auto directives = angel_lsp::utils::IncludeResolver::ExtractIncludes(request.sourceCode);

    for (const auto& diag : request.context.diagnostics)
    {
        if (!MatchDiagnosticCode(diag, "as-warn-include-not-found"))
        {
            continue;
        }
        const auto directive = std::find_if(directives.begin(), directives.end(),
                                            [&diag](const angel_lsp::utils::IncludeDirective& candidate)
                                            { return candidate.line == diag.range.start.line; });
        if (directive == directives.end() || directive->rawPath.empty())
        {
            continue;
        }

        auto ranked = RankIncludeCandidates(includerPath, directive->rawPath, indexedUris, request.uri);
        if (!ranked.empty())
        {
            EmitIncludeSuggestions(IncludeFixContext{request, diag, *directive}, ranked, actions);
        }
    }
}

void TryAddSortAndCleanIncludesAction(const CodeActionRequest& request, std::vector<lsp::CodeAction>& actions)
{
    auto includes = utils::IncludeResolver::ExtractIncludes(request.sourceCode);
    if (includes.empty())
    {
        return;
    }

    size_t firstLine = includes.front().line;
    size_t lastLine = includes.back().line;

    ankerl::unordered_dense::set<std::string> docReferences;
    auto rootScope = request.scopeIndex.GetRoot(request.uri);
    CollectAllReferences(rootScope.get(), docReferences);

    std::vector<std::string> angledIncludes;
    std::vector<std::string> quotedIncludes;
    ankerl::unordered_dense::set<std::string> seen;
    const IncludeRefCheckContext refCtx{request.uri, request.symbolTable, docReferences, request.allowedRoots};

    for (const auto& inc : includes)
    {
        if (seen.contains(inc.rawPath))
        {
            continue;
        }
        seen.insert(inc.rawPath);

        if (!IsIncludeDirectiveReferenced(inc, refCtx))
        {
            continue;
        }

        if (inc.isAngled)
        {
            angledIncludes.push_back(inc.rawPath);
        }
        else
        {
            quotedIncludes.push_back(inc.rawPath);
        }
    }

    std::sort(angledIncludes.begin(), angledIncludes.end());
    std::sort(quotedIncludes.begin(), quotedIncludes.end());

    lsp::TextEdit edit;
    edit.range.start = lsp::Position{static_cast<uint32_t>(firstLine), 0};
    edit.range.end = lsp::Position{static_cast<uint32_t>(lastLine + 1), 0};
    edit.newText = FormatSortedIncludesBlock(angledIncludes, quotedIncludes);

    lsp::CodeAction action;
    action.title = "Sort and Clean #include Directives";
    action.kind = lsp::CodeActionKindEnum(lsp::CodeActionKind::SourceOrganizeImports);

    lsp::WorkspaceEdit wsEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;
    changes[lsp::DocumentUri::parse(request.uri)] = {std::move(edit)};
    wsEdit.changes = std::move(changes);
    action.edit = std::move(wsEdit);

    actions.push_back(std::move(action));
}

} // namespace angel_lsp::features
