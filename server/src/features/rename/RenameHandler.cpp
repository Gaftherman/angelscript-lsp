#include "features/rename/RenameHandler.h"

#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TargetResolution.h"
#include "features/common/FeatureSharedUtils.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <vector>

namespace angel_lsp::features
{

/**
 * @brief Checks if all declarations of a class member across related classes reside in predefined headers.
 * @param[in] target Target symbol information.
 * @param[in] symbolTable Symbol table to look up declarations.
 * @param[in] isPredefinedUri Predicate reporting whether a document URI belongs to a predefined header.
 * @return True if at least one declaration was found and all declarations are in predefined headers.
 */
static bool AreClassMembersPredefined(const analysis::TargetDescriptor& target,
                                      const analysis::SymbolTable& symbolTable,
                                      const std::function<bool(std::string_view)>& isPredefinedUri)
{
    bool allPredefined = true;
    bool foundAny = false;
    for (const auto& cls : target.relatedClasses)
    {
        const auto syms = symbolTable.FindMemberSymbols(cls, target.name);
        for (const auto& sym : syms)
        {
            foundAny = true;
            if (!isPredefinedUri(sym.fileUri))
            {
                allPredefined = false;
                break;
            }
        }
        if (!allPredefined)
        {
            break;
        }
    }
    return foundAny && allPredefined;
}

/**
 * @brief Checks if all global declarations of a symbol reside in predefined headers.
 * @param[in] name Symbol name.
 * @param[in] symbolTable Symbol table to look up declarations.
 * @param[in] isPredefinedUri Predicate reporting whether a document URI belongs to a predefined header.
 * @return True if non-empty declarations were found and all reside in predefined headers.
 */
static bool AreGlobalSymbolsPredefined(std::string_view name, const analysis::SymbolTable& symbolTable,
                                       const std::function<bool(std::string_view)>& isPredefinedUri)
{
    const auto declSyms = symbolTable.FindSymbols(name);
    if (declSyms.empty())
    {
        return false;
    }
    for (const auto& sym : declSyms)
    {
        if (!isPredefinedUri(sym.fileUri))
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief Determines if the target symbol's definitions are located exclusively in predefined headers.
 * @param[in] target Target symbol information.
 * @param[in] symbolTable Symbol table to look up declarations.
 * @param[in] isPredefinedUri Predicate reporting whether a document URI belongs to a predefined header.
 * @return True if the symbol is predefined and should not be renamed.
 */
static bool IsTargetPredefined(const analysis::TargetDescriptor& target, const analysis::SymbolTable& symbolTable,
                               const std::function<bool(std::string_view)>& isPredefinedUri)
{
    if (target.kind == analysis::TargetKind::Local)
    {
        return isPredefinedUri(target.localUri);
    }
    if (target.kind == analysis::TargetKind::ClassMember)
    {
        return AreClassMembersPredefined(target, symbolTable, isPredefinedUri);
    }
    if (target.kind == analysis::TargetKind::EnumMember)
    {
        auto syms = symbolTable.FindSymbols(target.qualifiedName);
        for (const auto& s : syms)
        {
            if (isPredefinedUri(s.fileUri))
            {
                return true;
            }
        }
        return false;
    }
    return AreGlobalSymbolsPredefined(target.name, symbolTable, isPredefinedUri);
}

/**
 * @brief Builds a sorted LSP WorkspaceEdit from symbol occurrences and a replacement name.
 * @param[in] occurrences List of symbol locations across documents.
 * @param[in] newName New identifier name.
 * @return Populated and ordered WorkspaceEdit.
 */
static lsp::WorkspaceEdit BuildWorkspaceEdit(const std::vector<analysis::OccurrenceLocation>& occurrences,
                                             std::string_view newName)
{
    std::map<lsp::DocumentUri, std::vector<lsp::TextEdit>> editsByUri;
    for (const auto& loc : occurrences)
    {
        lsp::TextEdit edit;
        edit.range = lsp::Range{
            lsp::Position{loc.range.startLine, loc.range.startCharacter},
            lsp::Position{loc.range.endLine, loc.range.endCharacter},
        };
        edit.newText.assign(newName);
        editsByUri[lsp::DocumentUri::parse(loc.fileUri)].push_back(std::move(edit));
    }

    lsp::WorkspaceEdit workspaceEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;

    for (auto& [docUri, edits] : editsByUri)
    {
        std::sort(edits.begin(), edits.end(), [](const lsp::TextEdit& a, const lsp::TextEdit& b)
                  { return common::CompareByPosition(a.range, b.range); });

        changes[docUri] = std::move(edits);
    }

    workspaceEdit.changes = std::move(changes);
    return workspaceEdit;
}

/**
 * @brief Resolves a valid target for renaming under the cursor position.
 * @tparam RequestT Request type containing document URI, position, source text, and symbol table.
 * @param[in] request Rename or PrepareRename request payload.
 * @return Found valid rename target descriptor if present; std::nullopt otherwise.
 */
template <typename RequestT>
static std::optional<analysis::TargetDescriptor> ResolveValidRenameTarget(const RequestT& request)
{
    if (request.sourceCode.empty() || request.isPredefinedUri(request.uri))
    {
        return std::nullopt;
    }

    const auto target = analysis::ResolveTargetAt(
        request.uri, request.sourceCode, analysis::TargetPosition{request.position.line, request.position.character},
        request.symbolTable, request.scopeIndex, request.logger);

    if (!target.has_value())
    {
        return std::nullopt;
    }

    if (IsTargetPredefined(*target, request.symbolTable, request.isPredefinedUri))
    {
        return std::nullopt;
    }

    return target;
}

std::optional<lsp::PrepareRenameResult> PrepareRename(const PrepareRenameRequest& request)
{
    auto target = ResolveValidRenameTarget(request);
    if (!target.has_value())
    {
        return std::nullopt;
    }

    auto range = common::FindIdentifierRangeAtPosition(request.sourceCode, request.position);
    if (!range.has_value())
    {
        return std::nullopt;
    }

    lsp::PrepareRenamePlaceholder placeholder;
    placeholder.range = *range;
    placeholder.placeholder = target->name;

    return lsp::PrepareRenameResult(placeholder);
}

std::optional<lsp::WorkspaceEdit> Rename(const RenameRequest& request)
{
    if (!analysis::IsValidIdentifier(request.newName))
    {
        return std::nullopt;
    }

    auto target = ResolveValidRenameTarget(request);
    if (!target.has_value())
    {
        return std::nullopt;
    }

    auto occReq = analysis::MakeOccurrencesRequest(*target, request.uri, request.sourceCode, request.symbolTable,
                                                   request.scopeIndex);
    occReq.logger = request.logger;
    const auto occurrences = analysis::CollectOccurrences(occReq);

    if (occurrences.empty())
    {
        return std::nullopt;
    }

    return BuildWorkspaceEdit(occurrences, request.newName);
}
} // namespace angel_lsp::features
