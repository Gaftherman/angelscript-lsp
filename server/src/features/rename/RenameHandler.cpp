#include "features/rename/RenameHandler.h"
#include "features/symbol_resolution/SymbolResolution.h"
#include "analysis/SemanticHelpers.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
    // Everything this file used to define beyond these two entry points now lives in
    // features/symbol_resolution: find-references had a second copy of it, and the two
    // agreeing was left to whoever edited one of them remembering the other.
    using namespace angel_lsp::features::resolution;

    std::optional<lsp::PrepareRenameResult> PrepareRename(const PrepareRenameRequest &request)
    {
        if (request.predefinedUris.contains(request.uri))
        {
            return std::nullopt;
        }

        TSNode node{};
        auto target = ResolveTargetSymbol(
            request.uri,
            request.sourceCode,
            request.tree,
            request.position,
            request.symbolTable,
            request.scopeIndex,
            node);

        if (!target.has_value() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        // Check if the symbol definition is located in a predefined header
        if (target->kind == TargetKind::Local)
        {
            if (request.predefinedUris.contains(target->localUri))
            {
                return std::nullopt;
            }
        }
        else if (target->kind == TargetKind::ClassMember)
        {
            bool allPredefined = true;
            bool foundAny = false;
            for (const auto &cls : target->relatedClasses)
            {
                auto syms = request.symbolTable.FindSymbols(cls + "::" + target->name);
                for (const auto &sym : syms)
                {
                    foundAny = true;
                    if (!request.predefinedUris.contains(sym.fileUri))
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
            if (foundAny && allPredefined)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto declSyms = request.symbolTable.FindSymbols(target->name);
            if (!declSyms.empty())
            {
                bool allPredefined = true;
                for (const auto &sym : declSyms)
                {
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (allPredefined)
                {
                    return std::nullopt;
                }
            }
        }

        TSPoint startPt = ts_node_start_point(node);
        TSPoint endPt = ts_node_end_point(node);

        lsp::PrepareRenamePlaceholder placeholder;
        placeholder.range = lsp::Range{
            lsp::Position{ startPt.row, startPt.column },
            lsp::Position{ endPt.row, endPt.column }
        };
        placeholder.placeholder = target->name;

        return lsp::PrepareRenameResult(placeholder);
    }

    std::optional<lsp::WorkspaceEdit> Rename(const RenameRequest &request)
    {
        if (!IsValidIdentifier(request.newName))
        {
            return std::nullopt;
        }

        if (request.predefinedUris.contains(request.uri))
        {
            return std::nullopt;
        }

        TSNode node{};
        auto target = ResolveTargetSymbol(
            request.uri,
            request.sourceCode,
            request.tree,
            request.position,
            request.symbolTable,
            request.scopeIndex,
            node);

        if (!target.has_value() || ts_node_is_null(node))
        {
            return std::nullopt;
        }

        // Reject if target symbol is declared in a predefined header
        if (target->kind == TargetKind::Local)
        {
            if (request.predefinedUris.contains(target->localUri))
            {
                return std::nullopt;
            }
        }
        else if (target->kind == TargetKind::ClassMember)
        {
            bool allPredefined = true;
            bool foundAny = false;
            for (const auto &cls : target->relatedClasses)
            {
                auto syms = request.symbolTable.FindSymbols(cls + "::" + target->name);
                for (const auto &sym : syms)
                {
                    foundAny = true;
                    if (!request.predefinedUris.contains(sym.fileUri))
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
            if (foundAny && allPredefined)
            {
                return std::nullopt;
            }
        }
        else
        {
            auto declSyms = request.symbolTable.FindSymbols(target->name);
            if (!declSyms.empty())
            {
                bool allPredefined = true;
                for (const auto &sym : declSyms)
                {
                    if (!request.predefinedUris.contains(sym.fileUri))
                    {
                        allPredefined = false;
                        break;
                    }
                }
                if (allPredefined)
                {
                    return std::nullopt;
                }
            }
        }

        auto occurrences = CollectOccurrences(
            *target,
            request.uri,
            request.sourceCode,
            request.tree,
            request.symbolTable,
            request.scopeIndex);

        if (occurrences.empty())
        {
            return std::nullopt;
        }

        std::map<lsp::DocumentUri, std::vector<lsp::TextEdit>> editsByUri;
        for (const auto &loc : occurrences)
        {
            lsp::TextEdit edit;
            edit.range = loc.range;
            edit.newText = request.newName;
            editsByUri[loc.uri].push_back(std::move(edit));
        }

        lsp::WorkspaceEdit workspaceEdit;
        lsp::Map<lsp::DocumentUri, std::vector<lsp::TextEdit>> changes;

        for (auto &[docUri, edits] : editsByUri)
        {
            std::sort(edits.begin(), edits.end(),
                      [](const lsp::TextEdit &a, const lsp::TextEdit &b)
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
}
