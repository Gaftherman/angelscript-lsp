#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "analysis/resolution/TargetResolution.h"
#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <set>
#include <string>
#include <tree_sitter/api.h>
#include <unordered_set>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and immutable input parameters for a Document Highlight request.
 */
struct DocumentHighlightRequest
{
    std::string uri;
    std::string sourceCode;
    TSTree* tree = nullptr;
    lsp::Position position;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
};

/**
 * @brief List of document highlights in the current document.
 */
using DocumentHighlightResult = std::vector<lsp::DocumentHighlight>;

/**
 * @brief Resolves all read/write occurrences of the symbol under cursor in the current document.
 * @param request Immutable context for document highlight lookup.
 * @return Optional vector of DocumentHighlight items; nullopt if cursor symbol cannot be resolved.
 */
std::optional<DocumentHighlightResult> GetDocumentHighlights(const DocumentHighlightRequest& request);

/**
 * @brief Extracted callee callable metadata and argument count for call site highlighting.
 */
struct CalleeInfo
{
    std::string calleeName;
    std::string receiverType;
};

/**
 * @brief Accumulates document highlight occurrences and highlight kind markers.
 */
struct HighlightRangeCollector
{
    std::vector<lsp::Range> ranges;
    std::set<std::pair<uint32_t, uint32_t>> seen;

    /**
     * @brief Adds a range if not already collected.
     * @param[in] sL Start line.
     * @param[in] sC Start character.
     * @param[in] eL End line.
     * @param[in] eC End character.
     */
    void Add(uint32_t sL, uint32_t sC, uint32_t eL, uint32_t eC)
    {
        if (seen.insert({sL, sC}).second)
        {
            ranges.push_back(lsp::Range{lsp::Position{sL, sC}, lsp::Position{eL, eC}});
        }
    }
};

/**
 * @brief Context for highlighting class or struct member references across a document.
 */
struct MemberHighlightContext
{
    const analysis::TargetDescriptor& target;
    const std::unordered_set<std::string>& relatedSet;
    const DocumentHighlightRequest& request;
    const analysis::Scope* rootScope = nullptr;
};
} // namespace angel_lsp::features
