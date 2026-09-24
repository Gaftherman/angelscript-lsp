#include "features/rename/RenameHandler.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TargetResolution.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
using namespace angel_lsp::analysis;

namespace
{
/**
 * @brief Checks if all declarations of a class member across related classes reside in predefined headers.
 * @param[in] target Target symbol information.
 * @param[in] symbolTable Symbol table to look up declarations.
 * @param[in] predefinedUris Set of predefined document URIs.
 * @return True if at least one declaration was found and all declarations are in predefined headers.
 */
bool AreClassMembersPredefined(const TargetDescriptor& target, const analysis::SymbolTable& symbolTable,
                               const std::unordered_set<std::string>& predefinedUris)
{
    bool allPredefined = true;
    bool foundAny = false;
    for (const auto& cls : target.relatedClasses)
    {
        const auto syms = symbolTable.FindSymbols(cls + "::" + target.name);
        for (const auto& sym : syms)
        {
            foundAny = true;
            if (!predefinedUris.contains(sym.fileUri))
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
 * @param[in] predefinedUris Set of predefined document URIs.
 * @return True if non-empty declarations were found and all reside in predefined headers.
 */
bool AreGlobalSymbolsPredefined(const std::string& name, const analysis::SymbolTable& symbolTable,
                                const std::unordered_set<std::string>& predefinedUris)
{
    const auto declSyms = symbolTable.FindSymbols(name);
    if (declSyms.empty())
    {
        return false;
    }
    for (const auto& sym : declSyms)
    {
        if (!predefinedUris.contains(sym.fileUri))
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
 * @param[in] predefinedUris Set of predefined document URIs.
 * @return True if the symbol is predefined and should not be renamed.
 */
bool IsTargetPredefined(const TargetDescriptor& target, const analysis::SymbolTable& symbolTable,
                        const std::unordered_set<std::string>& predefinedUris)
{
    if (target.kind == TargetKind::Local)
    {
        return predefinedUris.contains(target.localUri);
    }
    if (target.kind == TargetKind::ClassMember)
    {
        return AreClassMembersPredefined(target, symbolTable, predefinedUris);
    }
    return AreGlobalSymbolsPredefined(target.name, symbolTable, predefinedUris);
}

/**
 * @brief Builds a sorted LSP WorkspaceEdit from symbol occurrences and a replacement name.
 * @param[in] occurrences List of symbol locations across documents.
 * @param[in] newName New identifier name.
 * @return Populated and ordered WorkspaceEdit.
 */
lsp::WorkspaceEdit BuildWorkspaceEdit(const std::vector<OccurrenceLocation>& occurrences, const std::string& newName)
{
    std::map<lsp::DocumentUri, std::vector<lsp::TextEdit>> editsByUri;
    for (const auto& loc : occurrences)
    {
        lsp::TextEdit edit;
        edit.range = lsp::Range{
            lsp::Position{loc.range.startLine, loc.range.startCharacter},
            lsp::Position{loc.range.endLine, loc.range.endCharacter},
        };
        edit.newText = newName;
        editsByUri[lsp::DocumentUri::parse(loc.fileUri)].push_back(std::move(edit));
    }

    lsp::WorkspaceEdit workspaceEdit;
    lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;

    for (auto& [docUri, edits] : editsByUri)
    {
        std::sort(edits.begin(), edits.end(),
                  [](const lsp::TextEdit& a, const lsp::TextEdit& b)
                  {
                      if (a.range.start.line != b.range.start.line)
                      {
                          return a.range.start.line < b.range.start.line;
                      }
                      return a.range.start.character < b.range.start.character;
                  });

        changes[docUri] = std::move(edits);
    }

    workspaceEdit.changes = std::move(changes);
    return workspaceEdit;
}
} // namespace

std::optional<lsp::PrepareRenameResult> PrepareRename(const PrepareRenameRequest& request)
{
    if (request.predefinedUris.contains(request.uri))
    {
        return std::nullopt;
    }

    TSNode node{};
    ResolveTargetRequest resolveReq{
        .uri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .position = TargetPosition{request.position.line, request.position.character},
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .outNode = node,
        .logger = request.logger,
    };
    const auto target = ResolveTargetSymbol(resolveReq);

    if (!target.has_value() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    if (IsTargetPredefined(*target, request.symbolTable, request.predefinedUris))
    {
        return std::nullopt;
    }

    const TSPoint startPt = ts_node_start_point(node);
    const TSPoint endPt = ts_node_end_point(node);

    lsp::PrepareRenamePlaceholder placeholder;
    placeholder.range = lsp::Range{lsp::Position{startPt.row, startPt.column}, lsp::Position{endPt.row, endPt.column}};
    placeholder.placeholder = target->name;

    return lsp::PrepareRenameResult(placeholder);
}

std::optional<lsp::WorkspaceEdit> Rename(const RenameRequest& request)
{
    if (!IsValidIdentifier(request.newName) || request.predefinedUris.contains(request.uri))
    {
        return std::nullopt;
    }

    TSNode node{};
    ResolveTargetRequest resolveReq{
        .uri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .position = TargetPosition{request.position.line, request.position.character},
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .outNode = node,
        .logger = request.logger,
    };
    const auto target = ResolveTargetSymbol(resolveReq);

    if (!target.has_value() || ts_node_is_null(node))
    {
        return std::nullopt;
    }

    if (IsTargetPredefined(*target, request.symbolTable, request.predefinedUris))
    {
        return std::nullopt;
    }

    CollectOccurrencesRequest occReq{
        .target = *target,
        .currentUri = request.uri,
        .sourceCode = request.sourceCode,
        .tree = request.tree,
        .symbolTable = request.symbolTable,
        .scopeIndex = request.scopeIndex,
        .includeDeclaration = true,
        .logger = request.logger,
    };
    const auto occurrences = CollectOccurrences(occReq);

    if (occurrences.empty())
    {
        return std::nullopt;
    }

    return BuildWorkspaceEdit(occurrences, request.newName);
}
} // namespace angel_lsp::features
