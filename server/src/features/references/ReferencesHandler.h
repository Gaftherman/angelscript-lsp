#pragma once

#include "analysis/index/ScopeTree.h"
#include "analysis/index/SymbolTable.h"
#include "utils/LspLogger.h"

#include <lsp/messages.h>
#include <lsp/types.h>
#include <optional>
#include <string_view>
#include <vector>

namespace angel_lsp::features
{
/**
 * @brief Context and input parameters for a Find References request.
 */
struct ReferencesRequest
{
    std::string_view uri;
    std::string_view sourceCode;
    lsp::Position position;
    bool includeDeclaration = true;
    const analysis::SymbolTable& symbolTable;
    const analysis::ScopeIndex& scopeIndex;
    angel_lsp::utils::LspLogger* logger = nullptr;
    std::optional<analysis::CallBindingRevision> callBindingRevision = std::nullopt;
    /// Module the occurrence search is restricted to; the viewed text must outlive the request.
    std::optional<std::string_view> occurrenceModule = std::nullopt;
};

using ReferencesResult = std::vector<lsp::Location>;

/**
 * @brief Resolves all declaration and reference occurrences of any target symbol
 *        (local variables, function parameters, class fields/methods, global declarations)
 * @param request Immutable context for references lookup.
 * @return Optional vector of Locations; nullopt if symbol cannot be resolved or no references found.
 */
std::optional<ReferencesResult> GetReferences(const ReferencesRequest& request);
} // namespace angel_lsp::features
