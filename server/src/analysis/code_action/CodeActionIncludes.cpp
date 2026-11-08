#include "analysis/code_action/CodeActionInternal.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Collects all indexed document URIs known to the symbol table.
 * @param[in] table Symbol table.
 * @return Set of known document URIs.
 */
static ankerl::unordered_dense::set<std::string> CollectIndexedFileUris(const analysis::SymbolTable& table)
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

/**
 * @brief Searches indexed document URIs for filenames similar to the unresolved include.
 * @param[in] includerPath Absolute filesystem path of current document.
 * @param[in] rawPath Unresolved include directive raw path.
 * @param[in] indexedUris Set of indexed file URIs.
 * @param[in] currentUri Current document URI.
 * @return Ranked candidates list sorted by edit distance.
 */
static std::vector<TypoCandidate> RankIncludeCandidates(const std::string& includerPath, const std::string& rawPath,
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
    std::vector<TypoCandidate> ranked;
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
 * @brief Emits quick-fix code action entries for unresolved include suggestions.
 * @param[in] ctx Include fix context.
 * @param[in] ranked Ranked include candidates.
 * @param[out] entries Destination code action entries vector.
 */
static void EmitIncludeSuggestions(const IncludeFixContext& ctx, const std::vector<TypoCandidate>& ranked,
                                   std::vector<CodeActionEntry>& entries)
{
    const std::string_view line =
        angel_lsp::utils::GetLine(ctx.query.sourceCode, static_cast<uint32_t>(ctx.directive.line));
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
        CodeActionEntry entry;
        entry.title = i18n::FormatMessage(ctx.query.i18n, "action-did-you-mean", "Did you mean '{}'?",
                                          std::string_view(ranked[i].name));
        entry.kind = CodeActionEntryKind::QuickFix;
        entry.diagnosticIndices = std::vector<uint32_t>{ctx.diagIndex};
        if (i == 0 && hasClearWinner)
        {
            entry.isPreferred = true;
        }

        entry.edits = std::vector<CodeActionTextEdit>{CodeActionTextEdit{
            analysis::SourceRange{static_cast<uint32_t>(ctx.directive.line), static_cast<uint32_t>(openPos + 1),
                                  static_cast<uint32_t>(ctx.directive.line), static_cast<uint32_t>(closePos)},
            ranked[i].name}};

        entries.push_back(std::move(entry));
    }
}

/**
 * @brief Checks whether an include directive is referenced by symbols used in the file.
 * @param[in] inc Include directive.
 * @param[in] ctx Include reference checking context.
 * @return True if include is referenced or has unknown symbols.
 */
static bool IsIncludeDirectiveReferenced(const angel_lsp::utils::IncludeDirective& inc,
                                         const IncludeRefCheckContext& ctx)
{
    std::string resolved = utils::IncludeResolver::ResolveIncludePath(utils::IncludeResolveRequest{
        .includePath = inc.rawPath, .currentFilePath = ctx.currentUri, .allowedRoots = ctx.allowedRoots});
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
static std::string FormatSortedIncludesBlock(const std::vector<std::string>& angledIncludes,
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

/**
 * @brief Generates quick-fix entries suggesting corrections for unresolved include directives.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddUnresolvedIncludeSuggestions(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    if (query.sourceCode.empty())
    {
        return;
    }
    const bool anyUnresolvedInclude =
        std::any_of(query.diagnostics.begin(), query.diagnostics.end(), [](const CodeActionDiagnosticView& diag)
                    { return MatchDiagnosticCode(diag, "as-warn-include-not-found"); });
    if (!anyUnresolvedInclude)
    {
        return;
    }
    const std::string includerPath = angel_lsp::utils::UriToPath(std::string(query.uri));
    if (includerPath.empty())
    {
        return;
    }

    auto indexedUris = CollectIndexedFileUris(query.symbolTable);
    const auto directives = angel_lsp::utils::IncludeResolver::ExtractIncludes(query.sourceCode);

    for (uint32_t i = 0; i < static_cast<uint32_t>(query.diagnostics.size()); ++i)
    {
        const auto& diag = query.diagnostics[i];
        if (!MatchDiagnosticCode(diag, "as-warn-include-not-found"))
        {
            continue;
        }
        const auto directive = std::find_if(directives.begin(), directives.end(),
                                            [&diag](const angel_lsp::utils::IncludeDirective& candidate)
                                            { return candidate.line == diag.range.startLine; });
        if (directive == directives.end() || directive->rawPath.empty())
        {
            continue;
        }

        auto ranked = RankIncludeCandidates(includerPath, directive->rawPath, indexedUris, std::string(query.uri));
        if (!ranked.empty())
        {
            EmitIncludeSuggestions(IncludeFixContext{query, i, *directive}, ranked, entries);
        }
    }
}

/**
 * @brief Generates a source action to sort and remove unreferenced include directives.
 * @param[in] query Incoming code action query context.
 * @param[in,out] entries Destination vector for generated code action entries.
 */
void TryAddSortAndCleanIncludesAction(const CodeActionQuery& query, std::vector<CodeActionEntry>& entries)
{
    auto includes = utils::IncludeResolver::ExtractIncludes(query.sourceCode);
    if (includes.empty())
    {
        return;
    }

    const size_t firstLine = includes.front().line;
    const size_t lastLine = includes.back().line;

    ankerl::unordered_dense::set<std::string> docReferences;
    auto rootScope = query.scopeIndex.GetRoot(query.uri);
    CollectAllReferences(rootScope.get(), docReferences);

    std::vector<std::string> angledIncludes;
    std::vector<std::string> quotedIncludes;
    ankerl::unordered_dense::set<std::string> seen;
    const IncludeRefCheckContext refCtx{std::string(query.uri), query.symbolTable, docReferences, query.allowedRoots};

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

    CodeActionEntry entry;
    entry.title =
        i18n::FormatMessage(query.i18n, "action-sort-and-clean-includes", "Sort and Clean #include Directives");
    entry.kind = CodeActionEntryKind::SourceOrganizeImports;
    entry.edits = std::vector<CodeActionTextEdit>{CodeActionTextEdit{
        analysis::SourceRange{static_cast<uint32_t>(firstLine), 0, static_cast<uint32_t>(lastLine + 1), 0},
        FormatSortedIncludesBlock(angledIncludes, quotedIncludes)}};

    entries.push_back(std::move(entry));
}

} // namespace angel_lsp::analysis::code_action
